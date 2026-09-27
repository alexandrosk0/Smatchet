// PendingActionQueueService bucket-A tests — the pending-action queue (Quality Pillar 6) on the
// in-memory fakes: FakeOfflineQueueDeps (FakeSyncCache + FakeTrackerClient, tasks run inline).
// Routing (offline queues with no request; online sends; a failure the tracker may still accept is
// queued), replay (sent once, the `sending` marker persisted first, a pass stops at a transport
// failure), the exactly-once checks for an interrupted comment, archiving, and the queue-panel
// actions. A per-case OfflineQueueTestEnvGuard sets read_only_mode=false in a temp config.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "Config/ConfigManager.h"
#include "OfflineQueueReplayPolicy.h"
#include "PendingActionPolicyPure.h"
#include "PendingActionTypes.h"
#include "Sync/PendingActionQueueService.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

const std::int64_t kQueuedAt = 1700000000;

struct Rig {
    FakeOfflineQueueDeps deps;
    PendingActionQueueService svc{deps};

    Rig() { deps.BackendImpl->EnableCollaboration(true); }

    smatchet_tests::FakeSyncCache& Cache() { return *deps.CacheImpl; }
    smatchet_tests::FakeTrackerClient& Tracker() { return *deps.BackendImpl; }

    std::int64_t QueueComment(const std::string& body, const char* state, const std::string& backendKey = "Jira") {
        return Cache().EnqueuePendingAction(backendKey, PendingActionKindWire(PendingActionKind::CommentAdd), "ABC-1",
                                            smatchet::pendingaction::BuildCommentActionPayload(body, kQueuedAt), state);
    }

    // One replay pass now: the first tick of a fresh service also loads the snapshot.
    void ReplayNow() {
        svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
        svc.RequestSnapshotRefresh();
        svc.Tick();
    }
};

TrackerIssueComment ServerComment(const std::string& body, std::int64_t createdAtSec) {
    TrackerIssueComment c;
    c.Id = "srv";
    c.Author = "Ana";
    c.Body = body;
    c.CreatedAtSec = createdAtSec;
    c.UpdatedAtSec = createdAtSec;
    return c;
}

std::string CommentPayload(const std::string& body) {
    return smatchet::pendingaction::BuildCommentActionPayload(body, kQueuedAt);
}

} // namespace

TEST_CASE("PendingActionQueueService::SubmitOrQueue offline saves the comment with no request") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const PendingActionSubmitResult r = rig.svc.SubmitOrQueue(
        PendingActionKind::CommentAdd, "ABC-1", CommentPayload("hi"), TrackerConnectivityState::TransportDown);
    CHECK(r.K == PendingActionSubmitResult::Kind::Queued);
    CHECK(r.QueueId > 0);
    CHECK_FALSE(r.QueuedAfterNetworkFailure);
    CHECK(rig.Tracker().AddCommentCalls().empty());
    const std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kPending);
    CHECK(rows[0].BackendKey == "Jira");
    CHECK(rows[0].IssueKey == "ABC-1");
    // The UI's view is published by the enqueue itself.
    CHECK(rig.svc.Snapshot()->Pending.size() == 1);
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue online sends once and queues nothing") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const PendingActionSubmitResult r =
        rig.svc.SubmitOrQueue(PendingActionKind::CommentAdd, "ABC-1", CommentPayload("hello"),
                              TrackerConnectivityState::AuthenticatedReachable);
    CHECK(r.K == PendingActionSubmitResult::Kind::Sent);
    REQUIRE(rig.Tracker().AddCommentCalls().size() == 1);
    CHECK(rig.Tracker().AddCommentCalls()[0].Body == "hello");
    CHECK(rig.Cache().LoadPendingActions().empty());
    CHECK(rig.deps.DeferredLiveNotifyCalls == 1);
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue queues a send that may have landed as ambiguous") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorTransport("operation timed out"));
    const PendingActionSubmitResult r =
        rig.svc.SubmitOrQueue(PendingActionKind::CommentAdd, "ABC-1", CommentPayload("maybe"),
                              TrackerConnectivityState::AuthenticatedReachable);
    CHECK(r.K == PendingActionSubmitResult::Kind::Queued);
    CHECK(r.QueuedAfterNetworkFailure);
    const std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kAmbiguous);
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue queues a rate-limited send as pending") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorRateLimited("slow down"));
    const PendingActionSubmitResult r =
        rig.svc.SubmitOrQueue(PendingActionKind::CommentAdd, "ABC-1", CommentPayload("later"),
                              TrackerConnectivityState::AuthenticatedReachable);
    CHECK(r.K == PendingActionSubmitResult::Kind::Queued);
    REQUIRE(rig.Cache().LoadPendingActions().size() == 1);
    CHECK(rig.Cache().LoadPendingActions()[0].State == PendingActionState::kPending);
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue reports a rejection and queues nothing") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorInvalidRequest("Comment body is too long", 400));
    const PendingActionSubmitResult r = rig.svc.SubmitOrQueue(
        PendingActionKind::CommentAdd, "ABC-1", CommentPayload("x"), TrackerConnectivityState::AuthenticatedReachable);
    CHECK(r.K == PendingActionSubmitResult::Kind::Failed);
    CHECK(r.Error == "Comment body is too long");
    CHECK(rig.Cache().LoadPendingActions().empty());
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue honours the Read-only preference") {
    OfflineQueueTestEnvGuard env;
    {
        std::ofstream f(ConfigManager::GetUserDataDirectory() + "smatchet_config.json",
                        std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":true}";
    }
    ConfigManager::InvalidateCache();
    Rig rig;
    const PendingActionSubmitResult r = rig.svc.SubmitOrQueue(
        PendingActionKind::CommentAdd, "ABC-1", CommentPayload("x"), TrackerConnectivityState::TransportDown);
    CHECK(r.K == PendingActionSubmitResult::Kind::Failed);
    CHECK(rig.Cache().LoadPendingActions().empty());
    CHECK(rig.Tracker().AddCommentCalls().empty());
}

TEST_CASE("PendingActionQueueService::SubmitOrQueue reports a local database failure") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.Cache().EnqueuePendingActionThrows = true;
    const PendingActionSubmitResult r = rig.svc.SubmitOrQueue(
        PendingActionKind::CommentAdd, "ABC-1", CommentPayload("x"), TrackerConnectivityState::TransportDown);
    CHECK(r.K == PendingActionSubmitResult::Kind::Failed);
    CHECK_FALSE(r.Error.empty());
}

TEST_CASE("PendingActionQueueService replay sends a queued comment once, marking it sending first") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t id = rig.QueueComment("queued offline", PendingActionState::kPending);
    rig.ReplayNow();
    REQUIRE(rig.Tracker().AddCommentCalls().size() == 1);
    CHECK(rig.Tracker().AddCommentCalls()[0].Body == "queued offline");
    CHECK(rig.Cache().LoadPendingActions().empty());
    CHECK(rig.svc.Snapshot()->Pending.empty());
    const auto history = rig.Cache().PendingActionStateHistory();
    REQUIRE_FALSE(history.empty());
    CHECK(history.front().first == id);
    CHECK(history.front().second == PendingActionState::kSending);
    // Nothing left: further ticks send nothing.
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().size() == 1);
}

TEST_CASE("PendingActionQueueService replay retires an ambiguous comment already on the tracker, with no POST") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("Fixed in *v2*", PendingActionState::kAmbiguous);
    rig.Tracker().SetIssueComments("ABC-1", {ServerComment("Fixed in _v2_", kQueuedAt + 2)});
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().empty());
    CHECK(rig.Tracker().FetchIssueCommentsCalls() == 1);
    CHECK(rig.Cache().LoadPendingActions().empty());
    CHECK(rig.Cache().LoadDeadPendingActions().empty());
}

TEST_CASE("PendingActionQueueService replay posts an ambiguous comment the tracker does not have, exactly once") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("not there yet", PendingActionState::kAmbiguous);
    rig.Tracker().SetIssueComments("ABC-1", {ServerComment("something else", kQueuedAt + 2)});
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().size() == 1);
    CHECK(rig.Cache().LoadPendingActions().empty());
}

TEST_CASE("PendingActionQueueService replay treats a row left `sending` as possibly sent") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("interrupted", PendingActionState::kSending);
    rig.Tracker().SetIssueComments("ABC-1", {ServerComment("interrupted", kQueuedAt)});
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().empty());
    CHECK(rig.Cache().LoadPendingActions().empty());
}

TEST_CASE("PendingActionQueueService replay archives a rejected comment") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t id = rig.QueueComment("rejected", PendingActionState::kPending);
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorInvalidRequest("Issue is closed", 400));
    rig.ReplayNow();
    CHECK(rig.Cache().LoadPendingActions().empty());
    const std::vector<DeadPendingAction> dead = rig.Cache().LoadDeadPendingActions();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].Row.Id == id);
    CHECK(dead[0].TerminalReason == "replay_rejected");
    CHECK(dead[0].Row.LastError == "Issue is closed");
    CHECK(rig.svc.Snapshot()->Dead.size() == 1);
}

TEST_CASE("PendingActionQueueService replay archives a comment at the attempt cap") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t id = rig.QueueComment("flaky", PendingActionState::kPending);
    rig.Cache().UpdatePendingAction(id, PendingActionState::kPending, OfflineQueueReplayPolicy::kMaxReplayAttempts - 1,
                                    "earlier failure");
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorRateLimited("slow down"));
    rig.ReplayNow();
    CHECK(rig.Cache().LoadPendingActions().empty());
    const std::vector<DeadPendingAction> dead = rig.Cache().LoadDeadPendingActions();
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].TerminalReason == "max_attempts");
    CHECK(dead[0].Row.Attempts == OfflineQueueReplayPolicy::kMaxReplayAttempts);
}

TEST_CASE("PendingActionQueueService replay stops at the first transport failure") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t first = rig.QueueComment("first", PendingActionState::kPending);
    const std::int64_t second = rig.QueueComment("second", PendingActionState::kPending);
    rig.Tracker().EnqueueAddCommentResult(TrackerErrorTransport("connection refused"));
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().size() == 1);
    const std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].Id == first);
    CHECK(rows[0].State == PendingActionState::kAmbiguous);
    CHECK(rows[0].Attempts == 1);
    CHECK(rows[1].Id == second);
    CHECK(rows[1].State == PendingActionState::kPending);
    CHECK(rows[1].Attempts == 0); // untouched: the tracker was unreachable
}

TEST_CASE("PendingActionQueueService replay never resends an interrupted worklog; it asks for review") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t id =
        rig.Cache().EnqueuePendingAction("Jira", PendingActionKindWire(PendingActionKind::WorklogAdd), "ABC-1",
                                         "{\"timeSpent\":\"1h\"}", PendingActionState::kAmbiguous);
    rig.ReplayNow();
    std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kNeedsReview);
    CHECK_FALSE(rows[0].LastError.empty());
    // Needs-review rows are not replayed; "Send again" re-queues it as pending with attempts reset.
    rig.ReplayNow();
    CHECK(rig.Cache().LoadPendingActions()[0].State == PendingActionState::kNeedsReview);
    rig.svc.SendAgain(id);
    rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kPending);
    CHECK(rows[0].Attempts == 0);
}

TEST_CASE("PendingActionQueueService replay never sends a comment discarded after the pass loaded it") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("keep", PendingActionState::kPending);
    const std::int64_t drop = rig.QueueComment("drop", PendingActionState::kPending);
    // Load the snapshot with replay held back, so the next load is the replay pass's own.
    rig.svc.PushReplayTimersForward(std::chrono::steady_clock::now() + std::chrono::hours(1));
    rig.svc.RequestSnapshotRefresh();
    rig.svc.Tick();
    REQUIRE(rig.Tracker().AddCommentCalls().empty());
    // The user discards `drop` after the pass copied the rows and before it reaches that row.
    rig.Cache().RunAfterNextLoadPendingActions([&rig, drop]() { rig.svc.Discard({drop}); });
    rig.svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    rig.svc.Tick();
    REQUIRE(rig.Tracker().AddCommentCalls().size() == 1);
    CHECK(rig.Tracker().AddCommentCalls()[0].Body == "keep");
    CHECK(rig.Cache().LoadPendingActions().empty());
    CHECK(rig.svc.Snapshot()->Pending.empty());
}

TEST_CASE("PendingActionQueueService::SendAgain leaves a row that does not need review on its own path") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t id = rig.QueueComment("maybe landed", PendingActionState::kAmbiguous);
    rig.Cache().UpdatePendingAction(id, PendingActionState::kAmbiguous, 2, "response lost");
    rig.svc.SendAgain(id);
    const std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kAmbiguous);
    CHECK(rows[0].Attempts == 2);
    // Replay still checks the tracker first, so a comment that did land is not posted twice.
    rig.Tracker().SetIssueComments("ABC-1", {ServerComment("maybe landed", kQueuedAt + 1)});
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().empty());
    CHECK(rig.Tracker().FetchIssueCommentsCalls() == 1);
    CHECK(rig.Cache().LoadPendingActions().empty());
}

TEST_CASE("PendingActionQueueService replay leaves another backend's rows queued") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("for plane", PendingActionState::kPending, "Plane");
    rig.ReplayNow();
    CHECK(rig.Tracker().AddCommentCalls().empty());
    CHECK(rig.Cache().LoadPendingActions().size() == 1);
}

TEST_CASE("PendingActionQueueService replay recovers after a failed launch") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    rig.QueueComment("after launch failure", PendingActionState::kPending);
    rig.ReplayNow(); // loads the snapshot and sends — reset to test a failing launch on a fresh row
    CHECK(rig.Tracker().AddCommentCalls().size() == 1);
    rig.QueueComment("second", PendingActionState::kPending);
    rig.svc.RequestSnapshotRefresh();
    rig.svc.Tick(); // reload the snapshot so the new row is visible
    const std::function<void(std::function<void()>)> inlineRunner = rig.deps.BackgroundTaskRunner;
    rig.deps.BackgroundTaskRunner = [](std::function<void()>) { throw std::runtime_error("no threads"); };
    rig.svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    CHECK_NOTHROW(rig.svc.Tick());
    rig.deps.BackgroundTaskRunner = inlineRunner;
    rig.svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    rig.svc.Tick(); // the in-flight latch was released: this pass runs
    CHECK(rig.Tracker().AddCommentCalls().size() == 2);
}

TEST_CASE("PendingActionQueueService queue-panel actions: discard, restore and delete") {
    OfflineQueueTestEnvGuard env;
    Rig rig;
    const std::int64_t keep = rig.QueueComment("keep", PendingActionState::kPending);
    const std::int64_t drop = rig.QueueComment("drop", PendingActionState::kPending);
    rig.svc.Discard({drop});
    std::vector<PendingActionRecord> rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Id == keep);
    CHECK(rig.svc.Snapshot()->Pending.size() == 1);

    rig.Cache().UpdatePendingAction(keep, PendingActionState::kAmbiguous, 3, "timeout");
    rig.Cache().ArchivePendingAction(keep, "max_attempts", std::string());
    rig.svc.RestoreDead({keep});
    rows = rig.Cache().LoadPendingActions();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].State == PendingActionState::kAmbiguous); // still checked before it is resent
    CHECK(rows[0].Attempts == 0);
    CHECK(rig.Cache().LoadDeadPendingActions().empty());

    rig.Cache().ArchivePendingAction(rows[0].Id, "replay_rejected", "closed");
    const std::vector<DeadPendingAction> dead = rig.Cache().LoadDeadPendingActions();
    REQUIRE(dead.size() == 1);
    rig.svc.DeleteDead({dead[0].DeadId});
    CHECK(rig.Cache().LoadDeadPendingActions().empty());
    CHECK(rig.svc.Snapshot()->Dead.empty());
}
