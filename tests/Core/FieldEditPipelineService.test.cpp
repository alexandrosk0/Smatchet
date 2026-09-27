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
#include "IssueTransitionsCacheService.h"
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

TrackerField MakeStatusField() {
    TrackerField f;
    f.Id = "status";
    f.Name = "Status";
    f.Family = TrackerFieldFamily::Status;
    TrackerFieldOption inProgress;
    inProgress.Id = "2";
    inProgress.Value = "In Progress";
    f.AllowedValueOptions.push_back(inProgress);
    return f;
}

// A status edit on ABC-1 (base "To Do"), kicked while the tracker was in `connectivity`.
FieldEditCommitRequest MakeStatusCommit(TrackerConnectivityState connectivity) {
    FieldEditCommitRequest req;
    req.IssueId = "ABC-1";
    req.Field = MakeStatusField();
    req.Values = {"2"};
    req.OriginalValue = "To Do";
    req.HasOriginalValue = true;
    req.IssueTypeKeySnapshot = "story";
    req.ConnectivityAtKick = connectivity;
    return req;
}

TrackerField MakeTimetrackingField() {
    TrackerField f;
    f.Id = "timeoriginalestimate";
    f.Name = "Original Estimate";
    f.Family = TrackerFieldFamily::Text;
    return f;
}

// Build the (deps, editMeta, transitions, service) trio with a single edited ticket already in the snapshot.
struct Rig {
    FakeFieldEditDeps fieldDeps;
    FakeEditMetaDeps editMetaDeps;
    EditMetaCacheService editMeta{editMetaDeps};
    IssueTransitionsCacheService transitions{editMetaDeps};
    FieldEditPipelineService svc{fieldDeps, editMeta, transitions};
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
    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", field, {"new summary"});

    CHECK(r.has_value());
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

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new"});

    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("Read-only") != std::string::npos);
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

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new"});

    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("edit metadata") != std::string::npos);
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

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeTextField("summary"), {"new summary"});

    CHECK(r.has_value());
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

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeSprintField(), {"42"});

    CHECK(r.has_value());
    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 1);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0); // sprint path never PUTs fields
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("customfield_sprint") == "Sprint 42");
    CHECK(rig.fieldDeps.DeferredNotifyCalls >= 1);
}

// The sprint + timetracking branch helpers migrated from `bool + outError` to VoidResult
// (build-quality-velocity-hardening #21). These pin the migrated Err paths as they surface
// back through the public SubmitFieldEdit VoidResult contract (#21 AppController flip — the public
// API is now VoidResult too, so the branch helpers' Err propagates directly, no bool adapter).
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit sprint branch rejects clearing (empty values)") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeSprintField(), {});

    CHECK_FALSE(r.has_value());
    CHECK(r.error() == "Clearing sprint is not supported by this action.");
    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 0); // rejected before any backend call
}

TEST_CASE("FieldEditPipelineService::SubmitFieldEdit timetracking branch rejects clearing (empty estimate)") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeTimetrackingField(), {});

    CHECK_FALSE(r.has_value());
    CHECK(r.error() == "Clearing Jira timetracking estimates is not supported by this editor.");
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
}

// ---------------------------------------------------------------------------
// (6) Timetracking branch — wraps the estimate into a `timetracking` payload.
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::SubmitFieldEdit timetracking branch writes a timetracking payload") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(true);

    const VoidResult r = rig.svc.SubmitFieldEdit("ABC-1", MakeTimetrackingField(), {"3d"});

    CHECK(r.has_value());
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
TEST_CASE("FieldEditPipelineService::SubmitFieldEditNetworkOnly classifies ErrorTransient from the "
          "mutation's TrackerError kind" *
          doctest::test_suite("[high-risk]")) {
    // N12 item 13b: the offline-queue fallback in the grid pipeline branches on
    // FieldEditResult::ErrorTransient, filled where the service flattens the mutation's
    // TrackerError — not by sniffing the flattened text.
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    const TrackerField field = MakeTextField("summary");

    SUBCASE("a retryable kind (5xx) marks the result transient even with bland text") {
        rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorServer("backend said no"));
        const FieldEditResult r = rig.svc.SubmitFieldEditNetworkOnly("ABC-1", field, {"v"}, "", "", "");
        CHECK_FALSE(r.Ok);
        CHECK(r.ErrorTransient);
        CHECK(r.Error == "backend said no");
    }
    SUBCASE("a non-retryable kind stays non-transient even with transport-shaped text") {
        rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(
            TrackerErrorInvalidRequest("Connection timeout while validating payload"));
        const FieldEditResult r = rig.svc.SubmitFieldEditNetworkOnly("ABC-1", field, {"v"}, "", "", "");
        CHECK_FALSE(r.Ok);
        CHECK_FALSE(r.ErrorTransient);
    }
    SUBCASE("local validation failures default to non-transient") {
        const FieldEditResult r = rig.svc.SubmitFieldEditNetworkOnly("", field, {"v"}, "", "", "");
        CHECK_FALSE(r.Ok);
        CHECK_FALSE(r.ErrorTransient);
    }
}

// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService offline-fallback prepare contract for a queueable field") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    const TrackerField field = MakeTextField("summary");

    // Step 1: the network-only path fails on a transport error.
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(false, "Could not resolve host (transport)");
    const FieldEditResult netResult = rig.svc.SubmitFieldEditNetworkOnly("ABC-1", field, {"new summary"}, "", "", "");
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

    const VoidResult r = rig.svc.ApplyFieldEditResult("ABC-1", result);

    CHECK(r.has_value());
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

    const VoidResult r = rig.svc.ApplyFieldEditResult("ABC-1", result);

    CHECK_FALSE(r.has_value());
    CHECK(r.error() == "backend rejected the edit");
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}

TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult fails when no cache is initialized") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.HasCacheImpl = false; // simulate cache-not-initialized

    FieldEditResult result;
    result.Ok = true;
    result.UpdatedDisplayValues["summary"] = "x";

    const VoidResult r = rig.svc.ApplyFieldEditResult("ABC-1", result);

    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("Local cache is unavailable") != std::string::npos);
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}

// ---------------------------------------------------------------------------
// (9) CommitOrQueue — the Quality Pillar 6 commit-or-queue seam (offline-first S6).
// ---------------------------------------------------------------------------
TEST_CASE("FieldEditPipelineService::FieldEditSupportsOfflineQueue accepts status edits") {
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeStatusField()));
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTextField("summary")));
    // Still excluded: sprint (AddIssueToSprint, not a field PUT) and editable estimates.
    CHECK_FALSE(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeSprintField()));
    CHECK_FALSE(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTimetrackingField()));
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues an offline edit with no network request" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"status", true}});
    TrackerConnectivityState offline = TrackerConnectivityState::TransportDown;
    SUBCASE("transport down") { offline = TrackerConnectivityState::TransportDown; }
    SUBCASE("service unavailable") { offline = TrackerConnectivityState::ServiceUnavailable; }

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(offline));

    CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
    CHECK(o.Error.empty());
    CHECK(o.QueueId == 1);
    CHECK_FALSE(o.QueuedAfterTransportFailure);
    // No update and no editmeta fetch: the edit never waits on the network while offline.
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    CHECK(rig.editMetaDeps.Fake()->FetchIssueEditMetaCallCount() == 0);
    REQUIRE(rig.fieldDeps.Enqueued.size() == 1);
    const FakeFieldEditDeps::EnqueuedEdit& queued = rig.fieldDeps.Enqueued.front();
    CHECK(queued.IssueKey == "ABC-1");
    CHECK(queued.FieldId == "status");
    CHECK_FALSE(queued.FieldsPayloadJson.empty());
    // The conflict base travels with the row so replay can detect a server-side move (ADR-0016).
    CHECK(queued.OriginalValue == "To Do");
    CHECK(queued.HasOriginalValue);
    // The optimistic local apply carries the new status straight away (the fake backend resolves
    // display values verbatim; JiraClient maps "2" to its option label).
    CHECK(o.Apply.Ok);
    REQUIRE(o.Apply.UpdatedDisplayValues.count("status") == 1);
    CHECK(o.Apply.UpdatedDisplayValues.at("status") == "2");
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues after a retryable failure online") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorTransport("connection refused"));
    FieldEditCommitRequest req = MakeStatusCommit(TrackerConnectivityState::AuthenticatedReachable);
    req.Field = MakeTextField("summary");
    req.Values = {"new summary"};

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);

    CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
    CHECK(o.QueuedAfterTransportFailure);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    // Only the network attempt fetched editmeta; preparing the queued edit fetched nothing more.
    CHECK(rig.editMetaDeps.Fake()->FetchIssueEditMetaCallCount() == 1);
    REQUIRE(rig.fieldDeps.Enqueued.size() == 1);
    CHECK(rig.fieldDeps.Enqueued.front().FieldId == "summary");
    CHECK(o.Apply.Ok);
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue reports a rejected edit and never queues it") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorInvalidRequest("status transition not allowed"));

    const FieldEditCommitOutcome o =
        rig.svc.CommitOrQueue(MakeStatusCommit(TrackerConnectivityState::AuthenticatedReachable));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error == "status transition not allowed");
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue reports why an offline edit could not be queued") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.EnqueueFailImpl = "Local cache is unavailable, so this edit cannot be queued offline.";

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(TrackerConnectivityState::TransportDown));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error == "Local cache is unavailable, so this edit cannot be queued offline.");
    CHECK(o.QueueId == 0);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue goes network-first when not known offline") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    rig.fieldDeps.Fake()->SetDefaultUpdateIssueFieldsResult(true);

    SUBCASE("unknown (before the first probe) saves online") {
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(TrackerConnectivityState::Unknown));
        CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
        CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    }
    SUBCASE("an offline edit the queue cannot hold (sprint) still goes to the network") {
        rig.fieldDeps.Fake()->SetDefaultAddIssueToSprintResult(true);
        FieldEditCommitRequest req = MakeStatusCommit(TrackerConnectivityState::TransportDown);
        req.Field = MakeSprintField();
        req.Values = {"42"};
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);
        CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
        CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 1);
    }
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue rejects every edit in read-only mode") {
    OfflineQueueTestEnvGuard env;
    {
        const std::string cfgPath = ConfigManager::GetUserDataDirectory() + "smatchet_config.json";
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":true}";
    }
    ConfigManager::InvalidateCache();
    Rig rig;
    rig.fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(TrackerConnectivityState::TransportDown));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error.find("Read-only") != std::string::npos);
    CHECK(rig.fieldDeps.Enqueued.empty());
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
}
