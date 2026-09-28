// offline_first.test.cpp — bucket-E tests for Quality Pillar 6 (offline-first).
//
// Runs against the offline-first fixture (tests/fixtures/jira_backend/offline-first.json) booted via
// SMATCHET_TEST_JIRA_BACKEND_FIXTURE; scripts/dev/test-ui-offline-first.sh sets it and the
// OfflineFirst filter. The fixture's FakeTrackerClient is attached to the process-wide
// GlobalFakeNetwork() switch (tests/support/FakeNetworkSwitch.h), so a test takes the tracker offline
// by flipping that switch and every network-shaped call fails like a real outage. Each test restores
// the switch with ScopedFakeNetworkReset.
//
// Tests are APP-STATE-COUPLED (like jira_deterministic_backend.test.cpp): they call the live
// AppController through SmatchetActiveUiTestAppController() and assert on its state, not on ImGui
// labels. Under any other fixture they skip with an informational log.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "Config/ConfigManager.h"
#include "DataFreshnessCue.h"
#include "PendingActionTypes.h"
#include "FakeNetworkSwitch.h"
#include "FakeTrackerClient.h"
#include "SmatchetCommentsModalUi.h" // OpenCommentsModal + GetCommentsModalSnapshotForTests
#include "SmatchetGridUiSupport.h"   // ProcessGridFieldEdits — the real grid commit pipeline
#include "SmatchetUiSession.h"       // g_ui, PendingFieldEdit
#include "Types/ConnectivityTypes.h"
#include "Types/ProjectComponentsTypes.h"
#include "Types/TransitionsTypes.h"
#include "UiTestWriteScope.h" // BucketE::UiTestWriteScope — the fresh profile defaults to read-only

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// True when the app was booted with the offline-first fixture; otherwise logs a SKIP.
bool OfflineFirstFixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "offline-first") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: offline-first fixture not active");
    return false;
}

// Yield one frame at a time until `done()` holds or `maxFrames` pass; returns the final `done()`.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

// Pull the connectivity probe forward every frame until the app reports `want` (the probe interval
// would otherwise make the test wait for the next scheduled probe).
bool WaitForConnectivity(ImGuiTestContext* ctx, AppController& app, TrackerConnectivityState want) {
    return YieldUntil(ctx, 600, [&app, want]() {
        if (app.GetLastTrackerConnectivityState() == want) {
            return true;
        }
        app.RequestTrackerProbeNow();
        return false;
    });
}

// The queued offline edit for (issueKey, fieldId), or null when none is queued.
const PendingFieldEditRecord* FindQueuedEdit(const std::vector<PendingFieldEditRecord>& rows, const char* issueKey,
                                             const char* fieldId) {
    const auto it = std::find_if(rows.begin(), rows.end(), [issueKey, fieldId](const PendingFieldEditRecord& r) {
        return r.IssueKey == issueKey && r.FieldId == fieldId;
    });
    return it == rows.end() ? nullptr : &*it;
}

// The structured comment thread saved with the in-memory ticket, or "" when there is none.
std::string SavedCommentThread(const AppController& app, const char* issueId) {
    const auto tickets = app.GetActiveTicketsSnapshot();
    if (!tickets) {
        return std::string();
    }
    const auto it = std::find_if(tickets->begin(), tickets->end(),
                                 [issueId](const CachedTicket& ticket) { return ticket.id == issueId; });
    return it == tickets->end() ? std::string() : it->GetFieldRichValue(kCommentThreadRichKey);
}

// Wait until the comments modal's load has ended (FetchInFlight released).
bool WaitForCommentsLoad(ImGuiTestContext* ctx) {
    return YieldUntil(ctx, 300, []() {
        const CommentsModalSnapshot snap = GetCommentsModalSnapshotForTests();
        return snap.Active && !snap.FetchInFlight;
    });
}

// Close the comments modal the way a user does (Esc with an empty draft). Not asserted: the modal
// only renders from a focused grid pane, and the next OpenCommentsModal resets its state anyway.
void CloseCommentsModalWithEscape(ImGuiTestContext* ctx) {
    ctx->KeyPress(ImGuiKey_Escape);
    if (!YieldUntil(ctx, 30, []() { return !GetCommentsModalSnapshotForTests().Active; })) {
        ctx->LogWarning("comments modal still open after Esc (grid pane not focused?)");
    }
}

TransitionsQuery MakeTransitionsQuery(const char* issueId, const char* issueType) {
    TransitionsQuery q;
    q.IssueId = issueId;
    q.ProjectKey = "OFF";
    q.IssueTypeKey = issueType;
    q.FromStatusKey = "1"; // "To Do" in the fixture catalog
    return q;
}

} // namespace

// OfflineFirst/Catalog_SurvivesTransportDown: a field-catalog refresh that fails because the tracker
// is unreachable keeps the catalog the user already has, raises no catalog error and shows a Warning
// (not Error) banner.
static void RegisterOfflineFirstCatalogSurvivesTransportDown(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Catalog_SurvivesTransportDown");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr); // surfaces as a test failure with context
            return;
        }
        const TrackerConfig cfg = ConfigManager::Load();

        // Online: the fixture's scripted catalog loads.
        IM_CHECK_NO_RET(app->RefreshFieldCatalog(cfg));
        IM_CHECK_NO_RET(!app->GetAvailableFields().empty());
        IM_CHECK_NO_RET(app->GetFieldCatalogError().empty());

        // Offline: the refresh fails at the transport level...
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(!app->RefreshFieldCatalog(cfg));
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() >= 1);

        // ...but the catalog the user already had survives, with a warning rather than an error.
        IM_CHECK_NO_RET(!app->GetAvailableFields().empty());
        IM_CHECK_NO_RET(app->GetFieldCatalogError().empty());
        IM_CHECK_NO_RET(app->GetTrackerConnectivityBannerForUi(nullptr).Kind ==
                        TrackerConnectivityBannerForUi::Level::Warning);
    };
}

// OfflineFirst/StatusCombo_OfflineShowsOptions: the status combo's transitions lookup never needs the
// network once a workflow edge was seen. Online, OFF-1's live transitions load (Fresh) and are
// remembered for (OFF, bug, To Do); offline, OFF-2 with the same project, type and status gets the
// remembered targets without any network call, and an unseen edge yields no options (the combo then
// lists every catalog status, covered by StatusComboOptionsPure). Drives the service through the app
// API, so no ImGui cell ids are needed.
static void RegisterOfflineFirstStatusComboOfflineShowsOptions(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "StatusCombo_OfflineShowsOptions");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        using smatchet::offline::DataFreshness;
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));

        // Online: the live transitions load in the background and are remembered.
        const TransitionsQuery online = MakeTransitionsQuery("OFF-1", "bug");
        app->EnsureIssueTransitionsLoaded(online);
        IM_CHECK_NO_RET(YieldUntil(ctx, 300, [app, &online]() {
            return app->GetAvailableTransitionsForIssue(online).freshness == DataFreshness::Fresh;
        }));
        const TransitionsLookup live = app->GetAvailableTransitionsForIssue(online);
        IM_CHECK_NO_RET(live.applicable);
        IM_CHECK_NO_RET(live.options.size() == 2);

        // Offline: wait until the app knows it, then look up an issue that was never fetched.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();

        const TransitionsQuery sameEdge = MakeTransitionsQuery("OFF-2", "bug");
        app->EnsureIssueTransitionsLoaded(sameEdge);
        const TransitionsQuery unseenEdge = MakeTransitionsQuery("OFF-9", "task");
        app->EnsureIssueTransitionsLoaded(unseenEdge);
        ctx->Yield(10);

        const TransitionsLookup learned = app->GetAvailableTransitionsForIssue(sameEdge);
        IM_CHECK_NO_RET(learned.applicable);
        IM_CHECK_NO_RET(learned.fromLearned);
        IM_CHECK_NO_RET(learned.options.size() == 2);
        IM_CHECK_NO_RET(learned.freshness == DataFreshness::CachedOffline);

        const TransitionsLookup unseen = app->GetAvailableTransitionsForIssue(unseenEdge);
        IM_CHECK_NO_RET(unseen.applicable);
        IM_CHECK_NO_RET(unseen.options.empty());

        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);

        // Leave the app online for the next test.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
    };
}

// OfflineFirst/StatusEdit_OfflineQueuesThenReplays: a status change made through the real grid commit
// pipeline while the tracker is unreachable is saved to the offline queue at once, with no network
// request, and is sent exactly once after the connection returns. Drives ProcessGridFieldEdits (as
// the debug.grid.edit-burst command does), so no ImGui cell ids are needed.
static void RegisterOfflineFirstStatusEditOfflineQueuesThenReplays(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "StatusEdit_OfflineQueuesThenReplays");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        BucketE::UiTestWriteScope writeScope; // the queue write must land: a rejected write would pass vacuously
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const std::shared_ptr<ITrackerBackend> backend = app->BackendShared();
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(backend.get());
        const TrackerField* statusField = app->FindFieldById("status");
        const auto tickets = app->GetActiveTicketsSnapshot();
        IM_CHECK_NO_RET(fake != nullptr);
        IM_CHECK_NO_RET(statusField != nullptr);
        IM_CHECK_NO_RET(tickets != nullptr);
        if (!fake || !statusField || !tickets) {
            return;
        }
        const auto ticketIt = std::find_if(tickets->begin(), tickets->end(),
                                           [](const CachedTicket& ticket) { return ticket.id == "OFF-1"; });
        IM_CHECK_NO_RET(ticketIt != tickets->end());
        IM_CHECK_NO_RET(FindQueuedEdit(app->GetPendingFieldEdits(), "OFF-1", "status") == nullptr);
        if (ticketIt == tickets->end()) {
            return;
        }

        // Offline, and the app knows it.
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();
        const std::size_t updatesBefore = fake->UpdateIssueFieldsCallCount();

        // "To Do" -> "In Progress" (option id "2"), with the scalar base the grid captures.
        PendingFieldEdit edit;
        edit.IssueId = "OFF-1";
        edit.Field = *statusField;
        edit.Values = {"2"};
        edit.OriginalValue = ticketIt->GetFieldValue("status");
        edit.HasOriginalValue = true;
        ProcessGridFieldEdits(*app, g_ui, {edit}, false);

        // Queued straight away, without trying the network first.
        IM_CHECK_NO_RET(YieldUntil(
            ctx, 120, [app]() { return FindQueuedEdit(app->GetPendingFieldEdits(), "OFF-1", "status") != nullptr; }));
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);

        // Back online: the recovery restarts the replay timers and the queued edit is sent once.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        const bool drained = YieldUntil(
            ctx, 900, [app]() { return FindQueuedEdit(app->GetPendingFieldEdits(), "OFF-1", "status") == nullptr; });
        if (!drained) {
            const std::vector<PendingFieldEditRecord> rows = app->GetPendingFieldEdits();
            const PendingFieldEditRecord* row = FindQueuedEdit(rows, "OFF-1", "status");
            ctx->LogError("queued status edit did not replay: connectivity=%d read_only=%d backend_key='%s' "
                          "attempts=%d conflict=%d last_error='%s'",
                          static_cast<int>(app->GetLastTrackerConnectivityState()),
                          ConfigManager::Load().ReadOnlyMode ? 1 : 0, row ? row->BackendKey.c_str() : "",
                          row ? row->Attempts : -1, row && row->HasMergeConflict ? 1 : 0,
                          row ? row->LastError.c_str() : "");
            if (row) {
                app->DeletePendingFieldEdits({row->Id}); // never leak the row into a retry or a later test
            }
        }
        IM_CHECK_NO_RET(drained);
        IM_CHECK_NO_RET(app->GetDeadPendingFieldEdits().empty());
        // The row is deleted only after UpdateIssueFields returned, so this read sees the call.
        IM_CHECK_NO_RET(fake->UpdateIssueFieldsCallCount() == updatesBefore + 1);
        if (fake->UpdateIssueFieldsCallCount() > updatesBefore) {
            // The fake's BuildFieldPayload encodes the edit as {"values": [...]}: the replay must send "2".
            const smatchet_tests::UpdateIssueFieldsCall& call = fake->UpdateIssueFieldsCalls().back();
            IM_CHECK_NO_RET(call.IssueId == "OFF-1");
            IM_CHECK_NO_RET(call.Fields.is_object() && call.Fields.contains("values"));
            IM_CHECK_NO_RET(call.Fields.value("values", nlohmann::json::array()) == nlohmann::json::array({"2"}));
        }
    };
}

// OfflineFirst/Comments_OfflineShowsCachedThread: the thread a sync or an online modal fetch saved
// with the ticket is what the comments modal shows offline — at once, marked as saved data, with no
// request made (it used to sit on "Loading comments..." and then say "No comments yet.").
static void RegisterOfflineFirstCommentsOfflineShowsCachedThread(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_OfflineShowsCachedThread");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const std::shared_ptr<ITrackerBackend> backend = app->BackendShared();
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(backend.get());
        IM_CHECK_NO_RET(fake != nullptr);
        if (!fake) {
            return;
        }

        // Online: the modal loads the live thread, which is saved with the ticket.
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        OpenCommentsModal(*app, "OFF-1");
        IM_CHECK_NO_RET(WaitForCommentsLoad(ctx));
        const CommentsModalSnapshot online = GetCommentsModalSnapshotForTests();
        IM_CHECK_NO_RET(!online.FetchFailed);
        IM_CHECK_NO_RET(!online.Seeded);
        IM_CHECK_NO_RET(online.CommentCount == 1);
        IM_CHECK_NO_RET(online.FirstAuthor == "Ana Offline");
        IM_CHECK_NO_RET(SavedCommentThread(*app, "OFF-1").find("Ana Offline") != std::string::npos);
        CloseCommentsModalWithEscape(ctx);

        // Offline: the saved thread shows straight away and nothing goes to the network.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();
        const std::size_t fetchesBefore = fake->FetchIssueCommentsCalls();
        OpenCommentsModal(*app, "OFF-1");
        IM_CHECK_NO_RET(WaitForCommentsLoad(ctx));
        const CommentsModalSnapshot offline = GetCommentsModalSnapshotForTests();
        IM_CHECK_NO_RET(offline.Active);
        IM_CHECK_NO_RET(offline.Seeded);
        IM_CHECK_NO_RET(!offline.SeedPartial); // the structured thread, not the tooltip summary
        IM_CHECK_NO_RET(offline.FetchFailed);  // the load skipped the network
        IM_CHECK_NO_RET(offline.CommentCount == 1);
        IM_CHECK_NO_RET(offline.FirstAuthor == "Ana Offline");
        IM_CHECK_NO_RET(fake->FetchIssueCommentsCalls() == fetchesBefore);
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);
        CloseCommentsModalWithEscape(ctx);

        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
    };
}

// OfflineFirst/Comments_PostedOfflineReplays: a comment posted while the tracker is unreachable is
// saved to the pending-action queue with no request, listed in the comments modal as waiting to sync,
// and posted exactly once after reconnecting.
static void RegisterOfflineFirstCommentsPostedOfflineReplays(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_PostedOfflineReplays");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        BucketE::UiTestWriteScope writeScope; // the queue write must land: a rejected write would pass vacuously
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const std::shared_ptr<ITrackerBackend> backend = app->BackendShared();
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(backend.get());
        IM_CHECK_NO_RET(fake != nullptr);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Pending.empty());
        if (!fake) {
            return;
        }

        // Offline: posting saves the comment and sends nothing.
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();
        const std::size_t postsBefore = fake->AddCommentCalls().size();
        const PendingActionSubmitResult queued =
            app->SubmitOrQueueComment(app->LatchPendingActionTarget(), "OFF-1", "offline hello");
        IM_CHECK_NO_RET(queued.K == PendingActionSubmitResult::Kind::Queued);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Pending.size() == 1);
        IM_CHECK_NO_RET(fake->AddCommentCalls().size() == postsBefore);
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);

        // The comments modal lists it as waiting to sync.
        OpenCommentsModal(*app, "OFF-1");
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().PendingCommentCount == 1);
        IM_CHECK_NO_RET(WaitForCommentsLoad(ctx));

        // Back online: the queue drains and the comment is posted exactly once.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        app->RetryOfflineQueuesNow();
        const bool drained =
            YieldUntil(ctx, 600, [app]() { return app->GetPendingActionsSnapshot()->Pending.empty(); });
        if (!drained) {
            const auto snap = app->GetPendingActionsSnapshot();
            const PendingActionRecord* row = snap->Pending.empty() ? nullptr : &snap->Pending.front();
            ctx->LogError("queued comment did not replay: connectivity=%d state='%s' attempts=%d last_error='%s'",
                          static_cast<int>(app->GetLastTrackerConnectivityState()), row ? row->State.c_str() : "",
                          row ? row->Attempts : -1, row ? row->LastError.c_str() : "");
            if (row) {
                app->DiscardPendingActions({row->Id}); // never leak the row into a retry or a later test
            }
        }
        IM_CHECK_NO_RET(drained);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Dead.empty());
        IM_CHECK_NO_RET(fake->AddCommentCalls().size() == postsBefore + 1);
        if (fake->AddCommentCalls().size() > postsBefore) {
            IM_CHECK_NO_RET(fake->AddCommentCalls().back().IssueKey == "OFF-1");
            IM_CHECK_NO_RET(fake->AddCommentCalls().back().Body == "offline hello");
        }
        CloseCommentsModalWithEscape(ctx);
    };
}

// OfflineFirst/Worklog_OfflineQueues: a worklog and a watch made while the tracker is unreachable are
// saved to the pending-action queue with no request, and each reaches the tracker exactly once after
// reconnecting.
static void RegisterOfflineFirstWorklogOfflineQueues(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Worklog_OfflineQueues");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        BucketE::UiTestWriteScope writeScope; // the queue write must land: a rejected write would pass vacuously
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const std::shared_ptr<ITrackerBackend> backend = app->BackendShared();
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(backend.get());
        IM_CHECK_NO_RET(fake != nullptr);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Pending.empty());
        if (!fake) {
            return;
        }

        // Offline: both are saved and nothing is sent.
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();
        const std::size_t worklogsBefore = fake->AddWorklogCalls().size();
        const std::size_t watchesBefore = fake->AddWatcherCalls().size();
        const PendingActionSubmitResult worklog =
            app->SubmitOrQueueWorklog(app->LatchPendingActionTarget(), "OFF-1", "30m", "", "auto", "offline work",
                                      "2026-09-27T10:00:00.000+0000");
        IM_CHECK_NO_RET(worklog.K == PendingActionSubmitResult::Kind::Queued);
        const PendingActionSubmitResult watch = app->SubmitOrQueueWatch(app->LatchPendingActionTarget(), "OFF-1");
        IM_CHECK_NO_RET(watch.K == PendingActionSubmitResult::Kind::Queued);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Pending.size() == 2);
        // The grid's Watch button reads this: it hides while the queued watch waits, whichever session queued it.
        IM_CHECK_NO_RET(app->FindQueuedPendingActionId(PendingActionKind::WatchAdd, "OFF-1") == watch.QueueId);
        IM_CHECK_NO_RET(app->FindQueuedPendingActionId(PendingActionKind::WatchAdd, "OFF-2") == 0);
        IM_CHECK_NO_RET(fake->AddWorklogCalls().size() == worklogsBefore);
        IM_CHECK_NO_RET(fake->AddWatcherCalls().size() == watchesBefore);
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);

        // Back online: the queue drains and each reaches the tracker exactly once.
        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::Up);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        app->RetryOfflineQueuesNow();
        const bool drained =
            YieldUntil(ctx, 600, [app]() { return app->GetPendingActionsSnapshot()->Pending.empty(); });
        if (!drained) {
            const auto snap = app->GetPendingActionsSnapshot();
            for (const PendingActionRecord& row : snap->Pending) {
                ctx->LogError("queued %s did not replay: state='%s' attempts=%d last_error='%s'", row.Kind.c_str(),
                              row.State.c_str(), row.Attempts, row.LastError.c_str());
            }
            std::vector<std::int64_t> ids;
            for (const PendingActionRecord& row : snap->Pending) {
                ids.push_back(row.Id);
            }
            app->DiscardPendingActions(ids); // never leak the rows into a retry or a later test
        }
        IM_CHECK_NO_RET(drained);
        IM_CHECK_NO_RET(app->GetPendingActionsSnapshot()->Dead.empty());
        IM_CHECK_NO_RET(app->FindQueuedPendingActionId(PendingActionKind::WatchAdd, "OFF-1") == 0);
        IM_CHECK_NO_RET(fake->AddWorklogCalls().size() == worklogsBefore + 1);
        if (fake->AddWorklogCalls().size() > worklogsBefore) {
            IM_CHECK_NO_RET(fake->AddWorklogCalls().back().IssueKey == "OFF-1");
            IM_CHECK_NO_RET(fake->AddWorklogCalls().back().TimeSpent == "30m");
            IM_CHECK_NO_RET(fake->AddWorklogCalls().back().Description == "offline work");
        }
        IM_CHECK_NO_RET(fake->AddWatcherCalls().size() == watchesBefore + 1);
        if (fake->AddWatcherCalls().size() > watchesBefore) {
            IM_CHECK_NO_RET(fake->AddWatcherCalls().back() == "OFF-1");
        }
    };
}

namespace {

bool HasOption(const ProjectComponentsLookup& lookup, const char* value) {
    return lookup.options != nullptr &&
           std::any_of(lookup.options->begin(), lookup.options->end(),
                       [value](const TrackerFieldOption& option) { return option.Value == value; });
}

bool HasUser(const AppController& app, const char* accountId, const char* displayName) {
    const std::vector<TrackerUser>& users = app.GetAvailableUsers();
    return std::any_of(users.begin(), users.end(), [accountId, displayName](const TrackerUser& user) {
        return user.AccountId == accountId && user.DisplayName == displayName;
    });
}

} // namespace

// OfflineFirst/Components_OfflineShowsSavedOptions: a project's component options, once fetched, stay on
// offer while the tracker is unreachable, with no request made, and a project with nothing saved says
// "not available yet" instead of spinning on "Loading components…" (the old editor spun forever). The
// next-session restore from the lookup cache is pinned in ProjectComponentsCacheService.test.cpp.
static void RegisterOfflineFirstComponentsOfflineShowsSavedOptions(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Components_OfflineShowsSavedOptions");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        using smatchet::offline::DataFreshness;

        // Online: the fixture's OFF components load (the post-sync warm may already have them).
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        IM_CHECK_NO_RET(YieldUntil(ctx, 300, [app]() {
            app->EnsureProjectComponentsLoaded("OFF");
            return app->GetComponentOptionsForProject("OFF").freshness == DataFreshness::Fresh;
        }));

        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();
        // What the grid does every frame for a components cell in each project.
        for (int frame = 0; frame < 10; ++frame) {
            app->EnsureProjectComponentsLoaded("OFF");
            app->EnsureProjectComponentsLoaded("NOPE");
            ctx->Yield();
        }

        const ProjectComponentsLookup off = app->GetComponentOptionsForProject("OFF");
        IM_CHECK_NO_RET(HasOption(off, "Backend"));
        IM_CHECK_NO_RET(HasOption(off, "Frontend"));
        const ProjectComponentsLookup none = app->GetComponentOptionsForProject("NOPE");
        IM_CHECK_NO_RET(none.options == nullptr);
        IM_CHECK_NO_RET(!none.inFlight);
        IM_CHECK_NO_RET(none.freshness == DataFreshness::UnavailableNoCache);
        IM_CHECK_NO_RET(std::strstr(DataFreshnessCue::CueText(none.freshness), "Loading") == nullptr);
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);
    };
}

// OfflineFirst/Users_RestoredFromSavedRoster: the user roster a live catalog fetch brought is saved to
// the local lookup cache (real SQLite), and a pane with no users gets it back from there while the
// tracker is unreachable, with no request made (the JQL autocomplete and assignee names used to go
// empty offline).
static void RegisterOfflineFirstUsersRestoredFromSavedRoster(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Users_RestoredFromSavedRoster");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        smatchet_tests::ScopedFakeNetworkReset reset;
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }

        // Online: a live catalog fetch through the grid path brings (and saves) the fixture roster.
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::AuthenticatedReachable));
        IM_CHECK_NO_RET(YieldUntil(ctx, 300, []() { return !g_ui.fieldCatalogLoading; }));
        g_ui.triggerCatalogRefetch = true;
        IM_CHECK_NO_RET(YieldUntil(ctx, 600, [app]() {
            return !g_ui.triggerCatalogRefetch && !g_ui.fieldCatalogLoading && HasUser(*app, "acc-ana", "Ana Offline");
        }));

        smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
        IM_CHECK_NO_RET(WaitForConnectivity(ctx, *app, TrackerConnectivityState::TransportDown));
        smatchet_tests::GlobalFakeNetwork().ResetCounters();

        // The pane loses its in-memory roster (as a tracker switch does); the saved copy comes back.
        // The save runs on a worker, so keep asking until the row is readable.
        app->SetAvailableUsers({});
        int frame = 0;
        const bool restored = YieldUntil(ctx, 600, [app, &frame]() {
            if (frame++ % 10 == 0) {
                app->SeedAvailableUsersFromStoreAsync();
            }
            return HasUser(*app, "acc-ana", "Ana Offline");
        });
        IM_CHECK_NO_RET(restored);
        IM_CHECK_NO_RET(HasUser(*app, "acc-bob", "Bob Offline"));
        IM_CHECK_NO_RET(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0);
    };
}

extern "C" void SmatchetRegisterOfflineFirstTests(ImGuiTestEngine* engine) {
    RegisterOfflineFirstCatalogSurvivesTransportDown(engine);
    RegisterOfflineFirstStatusComboOfflineShowsOptions(engine);
    RegisterOfflineFirstStatusEditOfflineQueuesThenReplays(engine);
    RegisterOfflineFirstCommentsOfflineShowsCachedThread(engine);
    RegisterOfflineFirstCommentsPostedOfflineReplays(engine);
    RegisterOfflineFirstWorklogOfflineQueues(engine);
    RegisterOfflineFirstComponentsOfflineShowsSavedOptions(engine);
    RegisterOfflineFirstUsersRestoredFromSavedRoster(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
