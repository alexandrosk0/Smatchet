// FieldEditPipelineService bucket-A tests — exercise the field-edit pipeline extracted from
// AppController (god-object decomposition Phase 2) through the IFieldEditDeps interface bundle.
// Each case builds a FakeFieldEditDeps + FakeEditMetaDeps + a real EditMetaCacheService (driven by
// the FakeEditMetaDeps) + a FieldEditPipelineService(fakeFieldEditDeps, editMeta). No AppController,
// ImGui, cpr, HTTP, or SQLite surface is touched — the fakes are header-only and SQLite-free.
//
// Every edit commits through CommitOrQueue (Quality Pillar 6: queue-first offline, network-first with
// a queue fallback online) and is applied with ApplyFieldEditResult. Requests are bound to the pane the
// user acted in (#2260); the pane-binding cases pin that a focus move never redirects an edit.
//
// A per-case OfflineQueueTestEnvGuard (OfflineQueueTestEnv.h) points ConfigManager at a temp dir with
// `read_only_mode=false` (a missing config defaults ReadOnlyMode=true, which blocks every edit) and
// redirects the BackendAuditTrail file there, so the read-only gate + audit writes do not depend on the
// developer's real config.
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
#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using smatchet_tests::FakeEditMetaDeps;
using smatchet_tests::FakeFieldEditDeps;
using smatchet_tests::FakeTrackerClient;
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

TrackerField MakeTimetrackingField(const std::string& id = "timeoriginalestimate") {
    TrackerField f;
    f.Id = id;
    f.Name = "Estimate";
    f.Family = TrackerFieldFamily::Text;
    return f;
}

// An edit of `field` on ABC-1, unbound (it binds to the focused pane), kicked while the tracker was in
// `connectivity`.
FieldEditCommitRequest MakeCommit(const TrackerField& field, const std::vector<std::string>& values,
                                  TrackerConnectivityState connectivity) {
    FieldEditCommitRequest req;
    req.IssueId = "ABC-1";
    req.Field = field;
    req.Values = values;
    req.OriginalValue = "To Do";
    req.HasOriginalValue = true;
    req.IssueTypeKeySnapshot = "story";
    req.Target.Connectivity = connectivity;
    return req;
}

// A status edit on ABC-1 (base "To Do").
FieldEditCommitRequest MakeStatusCommit(TrackerConnectivityState connectivity) {
    return MakeCommit(MakeStatusField(), {"2"}, connectivity);
}

const TrackerConnectivityState kOnline = TrackerConnectivityState::AuthenticatedReachable;
const TrackerConnectivityState kOffline = TrackerConnectivityState::TransportDown;

// The (deps, editMeta, transitions, service) set. Editmeta shares the field-edit fake backend, as the one
// production adapter does, so scripting either Fake() scripts the same tracker.
struct Rig {
    FakeFieldEditDeps fieldDeps;
    FakeEditMetaDeps editMetaDeps;
    EditMetaCacheService editMeta{editMetaDeps};
    IssueTransitionsCacheService transitions{editMetaDeps};
    FieldEditPipelineService svc{fieldDeps, editMeta, transitions};

    Rig() {
        editMetaDeps.BackendImpl = fieldDeps.BackendImpl;
        fieldDeps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1"));
    }

    // Commit online, then apply to the focused pane, as the grid and SubmitFieldEditOrQueue do.
    FieldEditCommitOutcome CommitAndApply(const FieldEditCommitRequest& req) {
        const FieldEditCommitOutcome o = svc.CommitOrQueue(req);
        if (o.Kind != FieldEditCommitKind::Failed) {
            CHECK(svc.ApplyFieldEditResult(fieldDeps.FocusedTarget(), req.IssueId, o.Apply).has_value());
        }
        return o;
    }
};

void WriteReadOnlyConfig() {
    const std::string cfgPath = ConfigManager::GetUserDataDirectory() + "smatchet_config.json";
    std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
    f << "{\"read_only_mode\":true}";
    f.close();
    ConfigManager::InvalidateCache();
}

} // namespace

TEST_CASE("FieldEditPipelineService::CommitOrQueue saves a regular edit online and applies it + notifies") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    const FieldEditCommitOutcome o = rig.CommitAndApply(MakeCommit(MakeTextField("summary"), {"new summary"}, kOnline));

    CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "new summary");
    CHECK(rig.fieldDeps.DeferredNotifyCalls >= 1);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue reports an edit editmeta denies and never queues it") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.editMetaDeps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", false}});

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeCommit(MakeTextField("summary"), {"new"}, kOnline));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error.find("edit metadata") != std::string::npos);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue retries after a 400 once editmeta is refreshed") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsFailure("HTTP 400 Bad Request");
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsSuccess();
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});

    const FieldEditCommitOutcome o = rig.CommitAndApply(MakeCommit(MakeTextField("summary"), {"new summary"}, kOnline));

    CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 2); // initial + retry
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "new summary");
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue adds to a sprint online and shows its name") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.Fake()->SetDefaultAddIssueToSprintResult(true);
    std::string value = "42";
    SUBCASE("by sprint id") { value = "42"; }
    SUBCASE("by sprint name") { value = "Sprint 42"; }

    const FieldEditCommitOutcome o = rig.CommitAndApply(MakeCommit(MakeSprintField(), {value}, kOnline));

    CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
    REQUIRE(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 1);
    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCalls().front().SprintId == "42");
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0); // sprint path never PUTs fields
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("customfield_sprint") == "Sprint 42");
    CHECK(rig.fieldDeps.DeferredNotifyCalls >= 1);
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue rejects clearing or an unknown sprint, online or offline") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    TrackerConnectivityState state = kOnline;
    SUBCASE("online") { state = kOnline; }
    SUBCASE("offline") { state = kOffline; }

    const FieldEditCommitOutcome cleared = rig.svc.CommitOrQueue(MakeCommit(MakeSprintField(), {}, state));
    CHECK(cleared.Kind == FieldEditCommitKind::Failed);
    CHECK(cleared.Error == "Clearing sprint is not supported by this action.");

    const FieldEditCommitOutcome unknown = rig.svc.CommitOrQueue(MakeCommit(MakeSprintField(), {"Backlog"}, state));
    CHECK(unknown.Kind == FieldEditCommitKind::Failed);
    CHECK(unknown.Error == "Unknown sprint: Backlog");

    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 0); // rejected before any backend call
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue sends both estimates in one timetracking payload") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    FieldEditCommitRequest req = MakeCommit(MakeTimetrackingField(), {"3d"}, kOnline);
    req.RemainingEstimateSnapshot = "1d";

    const FieldEditCommitOutcome o = rig.CommitAndApply(req);

    CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
    REQUIRE(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    const nlohmann::json& fields = rig.fieldDeps.Fake()->UpdateIssueFieldsCalls().front().Fields;
    CHECK(fields == nlohmann::json{{"timetracking", {{"originalEstimate", "3d"}, {"remainingEstimate", "1d"}}}});
    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("timeoriginalestimate") == "3d");
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("timeestimate") == "1d");
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue rejects clearing an estimate") {
    OfflineQueueTestEnvGuard env;
    Rig rig;

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeCommit(MakeTimetrackingField(), {}, kOnline));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error == "Clearing Jira timetracking estimates is not supported by this editor.");
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues on a retryable kind and reports the rest" *
          doctest::test_suite("[high-risk]")) {
    // N12 item 13b: the queue fallback branches on the mutation's TrackerError kind, never on the
    // flattened text.
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    const FieldEditCommitRequest req = MakeCommit(MakeTextField("summary"), {"v"}, kOnline);

    SUBCASE("a retryable kind (5xx) queues even with bland text") {
        rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorServer("backend said no"));
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);
        CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
        CHECK(o.QueuedAfterTransportFailure);
    }
    SUBCASE("a non-retryable kind is reported even with transport-shaped text") {
        rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(
            TrackerErrorInvalidRequest("Connection timeout while validating payload"));
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);
        CHECK(o.Kind == FieldEditCommitKind::Failed);
        CHECK(rig.fieldDeps.Enqueued.empty());
    }
    SUBCASE("a local validation failure is reported") {
        FieldEditCommitRequest noIssue = req;
        noIssue.IssueId.clear();
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(noIssue);
        CHECK(o.Kind == FieldEditCommitKind::Failed);
        CHECK(o.Error == "Issue id is empty.");
        CHECK(rig.fieldDeps.Enqueued.empty());
    }
}

TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult applies a successful result to the pane") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    FieldEditResult result;
    result.Ok = true;
    result.UpdatedDisplayValues["summary"] = "applied summary";

    SUBCASE("an unbound target applies to the focused pane") {
        CHECK(rig.svc.ApplyFieldEditResult(PendingActionTarget(), "ABC-1", result).has_value());
    }
    SUBCASE("a bound target applies to its own pane") {
        CHECK(rig.svc.ApplyFieldEditResult(rig.fieldDeps.FocusedTarget(), "ABC-1", result).has_value());
    }

    REQUIRE(rig.fieldDeps.UpdatedTickets.size() == 1);
    CHECK(rig.fieldDeps.UpdatedTickets.front().GetFieldValue("summary") == "applied summary");
}

TEST_CASE("FieldEditPipelineService::ApplyFieldEditResult rejects a not-ok result") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    FieldEditResult result;
    result.Ok = false;
    result.Error = "backend rejected the edit";

    const VoidResult r = rig.svc.ApplyFieldEditResult(PendingActionTarget(), "ABC-1", result);

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

    const VoidResult r = rig.svc.ApplyFieldEditResult(PendingActionTarget(), "ABC-1", result);

    CHECK_FALSE(r.has_value());
    CHECK(r.error().find("Local cache is unavailable") != std::string::npos);
    CHECK(rig.fieldDeps.UpdatedTickets.empty());
}

TEST_CASE("FieldEditPipelineService::FieldEditSupportsOfflineQueue covers status, sprint and estimate edits") {
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeStatusField()));
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTextField("summary")));
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeSprintField()));
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTimetrackingField("timeoriginalestimate")));
    CHECK(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTimetrackingField("timeestimate")));
    // Derived / worklog-backed time fields cannot be edited at all, so they never queue.
    CHECK_FALSE(FieldEditPipelineService::FieldEditSupportsOfflineQueue(MakeTimetrackingField("timespent")));
    TrackerField unknown = MakeTextField("customfield_opaque");
    unknown.Family = TrackerFieldFamily::Unknown;
    CHECK_FALSE(FieldEditPipelineService::FieldEditSupportsOfflineQueue(unknown));
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues an offline edit with no network request" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard env;
    Rig rig;
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
    CHECK(queued.BackendKey == "Jira");
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

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues an offline sprint edit as sprint_add" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard env;
    Rig rig;

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeCommit(MakeSprintField(), {"Sprint 42"}, kOffline));

    CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
    CHECK(rig.fieldDeps.Fake()->AddIssueToSprintCallCount() == 0);
    REQUIRE(rig.fieldDeps.Enqueued.size() == 1);
    CHECK(nlohmann::json::parse(rig.fieldDeps.Enqueued.front().FieldsPayloadJson) ==
          nlohmann::json{{"sprint_add", "42"}});
    CHECK(o.Apply.UpdatedDisplayValues.at("customfield_sprint") == "Sprint 42");
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues an offline estimate edit as timetracking" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    FieldEditCommitRequest req = MakeCommit(MakeTimetrackingField("timeestimate"), {"2h"}, kOffline);
    req.OriginalEstimateSnapshot = "1d";

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);

    CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    REQUIRE(rig.fieldDeps.Enqueued.size() == 1);
    CHECK(nlohmann::json::parse(rig.fieldDeps.Enqueued.front().FieldsPayloadJson) ==
          nlohmann::json{{"timetracking", {{"originalEstimate", "1d"}, {"remainingEstimate", "2h"}}}});
    CHECK(o.Apply.UpdatedDisplayValues.at("timeoriginalestimate") == "1d");
    CHECK(o.Apply.UpdatedDisplayValues.at("timeestimate") == "2h");
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue queues after a retryable failure online") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.editMetaDeps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorTransport("connection refused"));

    const FieldEditCommitOutcome o =
        rig.svc.CommitOrQueue(MakeCommit(MakeTextField("summary"), {"new summary"}, kOnline));

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
    rig.fieldDeps.Fake()->EnqueueUpdateIssueFieldsError(TrackerErrorInvalidRequest("status transition not allowed"));

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(kOnline));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error == "status transition not allowed");
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue reports why an offline edit could not be queued") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.fieldDeps.EnqueueFailImpl = "Local cache is unavailable, so this edit cannot be queued offline.";

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(kOffline));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error == "Local cache is unavailable, so this edit cannot be queued offline.");
    CHECK(o.QueueId == 0);
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue goes network-first when not known offline") {
    OfflineQueueTestEnvGuard env;
    Rig rig;

    SUBCASE("unknown (before the first probe) saves online") {
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(TrackerConnectivityState::Unknown));
        CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
        CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    }
    SUBCASE("an offline edit the queue cannot hold still goes to the network") {
        TrackerField opaque = MakeTextField("customfield_opaque");
        opaque.Family = TrackerFieldFamily::Unknown;
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeCommit(opaque, {"v"}, kOffline));
        CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
        CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 1);
    }
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CommitOrQueue rejects every edit in read-only mode") {
    OfflineQueueTestEnvGuard env;
    WriteReadOnlyConfig();
    Rig rig;

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(MakeStatusCommit(kOffline));

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error.find("Read-only") != std::string::npos);
    CHECK(rig.fieldDeps.Enqueued.empty());
    CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
}

// #2260: an edit is bound to the pane the user acted in. Here that pane ("board", tracker "Plane") is not
// the focused one — focus moved after the edit was latched — and nothing may reach the focused tracker.
TEST_CASE("FieldEditPipelineService keeps an edit on its latched pane after focus moves" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::shared_ptr<FakeTrackerClient> boundClient = std::make_shared<FakeTrackerClient>();
    boundClient->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    PendingActionTarget bound;
    bound.Backend = boundClient;
    bound.BackendKey = "Plane";
    bound.PaneId = "board";
    bound.BackendGeneration = 7;
    FieldEditCommitRequest req = MakeCommit(MakeTextField("summary"), {"moved"}, kOnline);

    SUBCASE("online: sent to, and checked against, the latched tracker only") {
        bound.Connectivity = kOnline;
        req.Target = bound;
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);
        CHECK(o.Kind == FieldEditCommitKind::SavedOnline);
        CHECK(boundClient->UpdateIssueFieldsCallCount() == 1);
        CHECK(boundClient->FetchIssueEditMetaCallCount() == 1);
        CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
        CHECK(rig.fieldDeps.Fake()->FetchIssueEditMetaCallCount() == 0);
    }
    SUBCASE("offline: queued under the latched tracker's namespace") {
        bound.Connectivity = kOffline;
        req.Target = bound;
        const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);
        CHECK(o.Kind == FieldEditCommitKind::QueuedOffline);
        REQUIRE(rig.fieldDeps.Enqueued.size() == 1);
        CHECK(rig.fieldDeps.Enqueued.front().BackendKey == "Plane");
        CHECK(boundClient->UpdateIssueFieldsCallCount() == 0);
        CHECK(rig.fieldDeps.Fake()->UpdateIssueFieldsCallCount() == 0);
    }
    SUBCASE("the local update never lands in the focused pane") {
        FieldEditResult saved;
        saved.Ok = true;
        saved.UpdatedDisplayValues["summary"] = "moved";
        CHECK(rig.svc.ApplyFieldEditResult(bound, "ABC-1", saved).has_value());
        CHECK(rig.fieldDeps.UpdatedTickets.empty());
    }
    SUBCASE("a pane that switched tracker since the edit gets no local update") {
        PendingActionTarget stale = rig.fieldDeps.FocusedTarget();
        stale.BackendGeneration = rig.fieldDeps.GenerationImpl + 1;
        FieldEditResult saved;
        saved.Ok = true;
        saved.UpdatedDisplayValues["summary"] = "stale";
        CHECK(rig.svc.ApplyFieldEditResult(stale, "ABC-1", saved).has_value());
        CHECK(rig.fieldDeps.UpdatedTickets.empty());
    }
}

TEST_CASE("FieldEditPipelineService refuses to queue an edit that names no tracker") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    FieldEditCommitRequest req = MakeStatusCommit(kOffline);
    req.Target.PaneId = "closed-pane"; // latched from a pane with no live context: no backend, no key
    req.Target.Connectivity = kOffline;

    const FieldEditCommitOutcome o = rig.svc.CommitOrQueue(req);

    CHECK(o.Kind == FieldEditCommitKind::Failed);
    CHECK(o.Error.find("not tied to a tracker") != std::string::npos);
    CHECK(rig.fieldDeps.Enqueued.empty());
}

TEST_CASE("FieldEditPipelineService::CaptureTicketSnapshots takes the base the pane shows") {
    CachedTicket ticket = MakeTicket("ABC-1", " Bug ");
    ticket.fieldValues["timeoriginalestimate"] = "1d";
    ticket.fieldValues["timeestimate"] = "4h";
    ticket.fieldValues["priority"] = "High";
    ticket.fieldRichValues["description"] = "{\"type\":\"doc\"}";

    FieldEditCommitRequest scalar;
    scalar.Field = MakeTextField("priority");
    FieldEditPipelineService::CaptureTicketSnapshots(ticket, true, scalar);
    CHECK(scalar.OriginalEstimateSnapshot == "1d");
    CHECK(scalar.RemainingEstimateSnapshot == "4h");
    CHECK(scalar.IssueTypeKeySnapshot == "bug");
    CHECK(scalar.OriginalValue == "High");
    CHECK(scalar.HasOriginalValue);
    CHECK(scalar.OriginalRichValue.empty());

    FieldEditCommitRequest rich;
    rich.Field = MakeTextField("description");
    FieldEditPipelineService::CaptureTicketSnapshots(ticket, true, rich);
    CHECK(rich.OriginalRichValue == "{\"type\":\"doc\"}");
    CHECK_FALSE(rich.HasOriginalValue);

    FieldEditCommitRequest keep;
    keep.Field = MakeTextField("priority");
    keep.OriginalValue = "Low";
    keep.HasOriginalValue = true;
    FieldEditPipelineService::CaptureTicketSnapshots(ticket, false, keep);
    CHECK(keep.OriginalValue == "Low"); // the grid's base, captured when the cell was edited, stays
    CHECK(keep.IssueTypeKeySnapshot == "bug");
}
