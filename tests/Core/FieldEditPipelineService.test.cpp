// FieldEditPipelineService bucket-A tests — exercise the field-edit NETWORK pipeline extracted
// from AppController (god-object decomposition Phase 2) through the IFieldEditDeps interface bundle.
// Each case builds a FakeFieldEditDeps + FakeEditMetaDeps + a real EditMetaCacheService (driven by
// the FakeEditMetaDeps) + a FieldEditPipelineService(fakeFieldEditDeps, editMeta). No AppController,
// ImGui, cpr, HTTP, or SQLite surface is touched — the fakes are header-only and SQLite-free.
//
// A per-case OfflineQueueTestEnvGuard (OfflineQueueTestEnv.h) points ConfigManager at a temp dir with
// `read_only_mode=false` (a missing config defaults ReadOnlyMode=true, which blocks every edit) and
// redirects the BackendAuditTrail file there, so SubmitFieldEdit's read-only gate + audit writes do
// not depend on the developer's real config.
//
// Per-case isolation: every TEST_CASE constructs its own fakes + fresh services. No statics, no
// shared world state, no order dependencies (the OfflineQueueTestEnvGuard's process-wide config is set+reset
// per case; doctest's single-threaded runner prevents races).

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeFieldEditDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h" // OfflineQueueTestEnvGuard (read_only_mode=false + audit temp dir)

#include "CachedTicketTypes.h"
#include "EditMetaCacheService.h"
#include "FieldEditPipelineService.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/FieldEditTypes.h"

#include "Config/ConfigManager.h"

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

using smatchet_tests::FakeEditMetaDeps;
using smatchet_tests::FakeFieldEditDeps;
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

CachedTicket MakeTicket(const std::string& id, const std::string& issueType = "story") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "old summary";
    t.fieldValues["issuetype"] = issueType;
    return t;
}

TrackerField MakeTextField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    f.Name = id;
    f.Family = TrackerFieldFamily::Text;
    return f;
}

TrackerField MakeSprintField() {
    TrackerField f;
    f.Id = "customfield_sprint";
    f.Name = "Sprint";
    f.Family = TrackerFieldFamily::Sprint;
    // A sprint option so the optimistic display value resolves to the option label.
    TrackerFieldOption opt;
    opt.Id = "42";
    opt.Value = "Sprint 42";
    f.AllowedValueOptions.push_back(opt);
    return f;
}

TrackerField MakeTimetrackingField() {
    TrackerField f;
    f.Id = "timeoriginalestimate";
    f.Name = "Original Estimate";
    f.Family = TrackerFieldFamily::Text;
    return f;
}

// Build the (deps, editMeta, service) trio with a single edited ticket already in the snapshot.
struct Rig {
    FakeFieldEditDeps fieldDeps;
    FakeEditMetaDeps editMetaDeps;
    EditMetaCacheService editMeta{editMetaDeps};
    FieldEditPipelineService svc{fieldDeps, editMeta};
};

} // namespace

// ---------------------------------------------------------------------------
// (1) Regular field success → UpdateTicket + deferred notify.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit regular success applies optimistic update + notify") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(true); // PUT succeeds
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    const TrackerField field = MakeTextField("summary");
    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", field, {"new summary"}, err);

    CHECK(ok);
    CHECK(err.empty());
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "new summary");
    CHECK(rig.fieldDeps.DeferredNotifyCalls >= 1);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
}

// ---------------------------------------------------------------------------
// (2) Read-only mode blocks before any network call.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit blocked by read-only mode") {
    OfflineQueueTestEnvGuard env;
    // Flip the temp config to read-only AFTER the guard wrote read_only_mode=false (same temp dir
    // the guard pointed ConfigManager at; restored on guard teardown).
    {
        const std::string cfgPath = ConfigManager::GetUserDataDirectory() + "smatchet_config.json";
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":true}";
    }
    ConfigManager::InvalidateCache();

    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new"}, err);

    CHECK_FALSE(ok);
    CHECK(err.find("Read-only") != std::string::npos);
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
}

// ---------------------------------------------------------------------------
// (3) Editmeta-deny blocks the regular branch (no PUT issued).
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit blocked when editmeta denies the field") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    // Loaded editmeta that explicitly DENIES summary.
    rig.editMetaDeps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", false}});

    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new"}, err);

    CHECK_FALSE(ok);
    CHECK(err.find("edit metadata") != std::string::npos);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}

// ---------------------------------------------------------------------------
// (4) 400 → refresh editmeta (now allows) → retry → succeed (cross-service).
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit retries after a 400 once editmeta is refreshed") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    // First PUT returns HTTP 400; second PUT (after editmeta refresh) succeeds.
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsFailure("HTTP 400 Bad Request");
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsSuccess();
    // editmeta allows summary throughout (the regular branch's pre-check + the post-400 re-check).
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new summary"}, err);

    CHECK(ok);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 2); // initial + retry
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "new summary");
}

// ---------------------------------------------------------------------------
// (5) Sprint branch — AddIssueToSprint + optimistic option-label display value.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit sprint branch adds to sprint + resolves option label") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->SetDefaultAddIssueToSprintResult(true);

    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", MakeSprintField(), {"42"}, err);

    CHECK(ok);
    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 1);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0); // sprint path never PUTs fields
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("customfield_sprint") == "Sprint 42");
    CHECK(rig.fieldDeps.DeferredNotifyCalls >= 1);
}

// ---------------------------------------------------------------------------
// (6) Timetracking branch — wraps the estimate into a `timetracking` payload.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit timetracking branch writes a timetracking payload") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(true);

    std::string err;
    const bool ok = rig.svc.SubmitFieldEdit("ABC-1", MakeTimetrackingField(), {"3d"}, err);

    CHECK(ok);
    REQUIRE(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    const auto& call = rig.fieldDeps.Fake()->UpdateIssueFieldsCalls().front();
    REQUIRE(call.Fields.contains("timetracking"));
    CHECK(call.Fields["timetracking"]["originalEstimate"] == "3d");
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("timeoriginalestimate") == "3d");
}

// ---------------------------------------------------------------------------
// (7) Offline-fallback PREPARE contract. The DB-row write is grid-layer (out of scope) — assert:
//     NetworkOnly fails on a transport error → false; FieldEditSupportsOfflineQueue == true for a
//     queueable field; TryPrepareOfflineFieldEdit → true with a non-empty payload JSON.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService offline-fallback prepare contract for a queueable field") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    const TrackerField field = MakeTextField("summary");

    // Step 1: the network-only path fails on a transport error.
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(false, "Could not resolve host (transport)");
    FieldEditResult netResult;
    const bool netOk = rig.svc.SubmitFieldEditNetworkOnly("ABC-1", field, {"new summary"}, "", "", "", netResult);
    CHECK_FALSE(netOk);
    CHECK_FALSE(netResult.Ok);

    // Step 2: the field is offline-queueable (Text family, not sprint/timetracking).
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(field));

    // Step 3: prepare the offline payload — builds JSON (BuildFieldPayload) WITHOUT a network PUT.
    FieldEditResult prepResult;
    std::string payloadJson;
    std::string prepErr;
    const bool prepOk = rig.svc.TryPrepareOfflineFieldEdit("ABC-1", field, {"new summary"}, "", "", "", prepResult,
                                                           payloadJson, prepErr);
    CHECK(prepOk);
    CHECK(prepResult.Ok);
    CHECK(prepErr.empty());
    CHECK_FALSE(payloadJson.empty());
    // The DB queue write itself (QueueFieldEditOffline) is grid-layer — NOT asserted here.
}

// ---------------------------------------------------------------------------
// (8) ApplyFieldEditResult — success / not-ok / no-cache.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult applies a successful result to the cache") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    FieldEditResult result;
    result.Ok = true;
    result.UpdatedDisplayValues["summary"] = "applied summary";

    std::string err;
    const bool ok = rig.svc.ApplyFieldEditResult("ABC-1", result, err);

    CHECK(ok);
    CHECK(err.empty());
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "applied summary");
}

TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult rejects a not-ok result") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    FieldEditResult result;
    result.Ok = false;
    result.Error = "backend rejected the edit";

    std::string err;
    const bool ok = rig.svc.ApplyFieldEditResult("ABC-1", result, err);

    CHECK_FALSE(ok);
    CHECK(err == "backend rejected the edit");
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}

TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult fails when no cache is initialized") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.HasCacheImpl = false; // simulate cache-not-initialized

    FieldEditResult result;
    result.Ok = true;
    result.UpdatedDisplayValues["summary"] = "x";

    std::string err;
    const bool ok = rig.svc.ApplyFieldEditResult("ABC-1", result, err);

    CHECK_FALSE(ok);
    CHECK(err.find("Local cache is unavailable") != std::string::npos);
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}
