// Issue #1081 regression coverage — backend switch (Jira→GitHub) raced in-flight sync
// workers into ActiveTickets → std::terminate. Four fixes pinned here:
//   1. Publish-under-lock: SwapBackendIfTrackerChanged's backend-kind clear+publish happens
//      under ONE ActiveTicketsMutex scope (the unguarded publish was the primary terminate
//      candidate — shared_ptr control-block UB against worker writers + locked readers).
//   2. Capture-then-check generation token: a replay that completed against the OLD backend
//      drops its post-replay RefreshLocalData when GridLiveContext::backendGeneration_ moved
//      mid-flight (IOfflineQueueDeps::BackendGeneration seam).
//   3. Replay key latch: TickOfflineCreates passes the key CAPTURED at work-capture time into
//      IssueCreatePipeline::Run — a mid-replay key switch can no longer seed a Jira row into
//      the GitHub cache namespace (proven TOCTOU).
//   4. Sync-storm damping + stale-kick drop: the pure decision helpers in PaneSyncKickPolicy.h
//      (ShouldKickInitialSync / PaneSyncKickStillCurrent). AppController's wiring of these is
//      not unit-reachable (no AppController test rig) — the extracted decision logic is tested
//      instead, per the BackgroundWorkerReap.h precedent; the stale-cfg no-reswap behaviour
//      ("Create('Jira') never called after a GitHub switch") is exactly the
//      PaneSyncKickStillCurrent==false branch dropping SyncPaneWithBackend.
//
// Per-case isolation: every TEST_CASE owns its fixtures (in-memory FakeSyncCache, fresh service).

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTicketSyncDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "ConfigManager.h"
#include "IssueDraft.h"
#include "OfflineQueueService.h"
#include "PaneSyncKickPolicy.h"
#include "TicketSyncService.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::FakeTicketSyncDeps;
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

IssueDraft MakeDraft(const std::string& summary) {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = summary;
    return d;
}

void PrimeCreateHappy(FakeOfflineQueueDeps& deps) {
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "X"}}}});
    TrackerField f;
    f.Id = "summary";
    f.Name = "summary";
    f.Type = "string";
    deps.Fields = {f};
}

} // namespace

TEST_CASE("issue #1081: backend-kind swap publishes the cleared ActiveTickets snapshot UNDER the mutex" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeTicketSyncDeps deps; // default backend reports Jira
    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "GitHub"; // kind change → backendSwapped branch: clear + publish empty
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    // The swap publish runs synchronously inside SyncWithBackend (before the worker spawns),
    // so every publish counted here came from the swap path on this thread. Pre-fix the
    // publish sat OUTSIDE the ActiveTicketsMutex scope → the fake's cross-thread try_lock
    // probe sees the mutex free → PublishCallsUnderLock < PublishCalls.
    CHECK(deps.PublishCalls >= 1);
    CHECK(deps.PublishCallsUnderLock == deps.PublishCalls);
    REQUIRE(deps.ActiveTicketsPublishedImpl);
    CHECK(deps.ActiveTicketsPublishedImpl->empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("issue #1081: offline field-edit replay drops RefreshLocalData when the generation moved mid-replay" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", "{\"summary\":\"v\"}", err, std::string()) > 0);
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(captured); // generation captured at tick; apply deferred — the swap window is open

    deps.BackendGenerationImpl = 1; // backend swapped between capture and apply
    captured();

    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u); // the replay itself completed
    // The stale wholesale ActiveTickets replace was dropped: in production RefreshLocalData
    // is what replaces ActiveTickets + republishes the snapshot + bumps the revision — not
    // calling it means snapshot and revision stay untouched.
    CHECK(deps.RefreshLocalDataCalls == 0);
}

TEST_CASE("issue #1081 control: offline field-edit replay still refreshes when the generation is unchanged") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", "{\"summary\":\"v\"}", err, std::string()) > 0);
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(captured);

    captured(); // generation unchanged → normal refresh

    CHECK(deps.RefreshLocalDataCalls == 1);
    // And the refresh went through the CHECKED overload — the captured generation must reach
    // the apply-time under-lock re-check, not just the worker-side pre-check.
    CHECK(deps.CheckedRefreshLocalDataCalls == 1);
}

TEST_CASE("issue #1081: post-replay refresh is dropped when the generation moves MID-refresh "
          "(after the worker pre-check, before the locked swap-in)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };
    // The swap lands INSIDE the refresh window: the worker-side pre-check passes (generation
    // still 0), then the backend swaps during the full-table cache read — modelled by bumping
    // the generation from the checked overload's entry hook, before its apply-time re-check.
    // Pre-fix the service called the UNCHECKED RefreshLocalData() here, so this window
    // wholesale-replaced the NEW backend's ActiveTickets with OLD-key rows (TOCTOU re-open).
    deps.OnCheckedRefreshLocalData = [&deps]() { deps.BackendGenerationImpl = 1; };

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", "{\"summary\":\"v\"}", err, std::string()) > 0);
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(captured);

    captured(); // generation 0 at the pre-check → proceeds into the checked refresh

    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u); // the replay itself completed
    CHECK(deps.CheckedRefreshLocalDataCalls == 1);               // checked overload was used
    CHECK(deps.CheckedRefreshLocalDataDrops == 1);               // apply-time re-check dropped it
    CHECK(deps.RefreshLocalDataCalls == 0);                      // no wholesale replace applied
}

TEST_CASE("issue #1081: offline create replay writes under the CAPTURED key after a mid-replay key switch" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps; // context key "Jira"
    PrimeCreateHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-9");

    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeDraft("key latch victim")) > 0); // queued under "Jira"
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();
    REQUIRE(captured); // key + generation captured at tick; the swap window is open

    // Mid-replay backend switch: the live key flips to GitHub and the generation moves.
    deps.CacheBackendKeyImpl = "GitHub";
    deps.BackendGenerationImpl = 1;
    captured();

    CHECK(deps.CacheImpl->LoadPendingCreates().empty()); // row consumed (successful replay)
    // The seeded row landed under the key the row was QUEUED against — never the post-switch
    // key (pre-fix: a write-time CacheBackendKey() re-read put this Jira row under "GitHub").
    CHECK(deps.CacheImpl->GetAllTickets("Jira").size() == 1u);
    CHECK(deps.CacheImpl->GetAllTickets("GitHub").empty());
    // And the stale post-replay refresh was dropped (generation mismatch).
    CHECK(deps.RefreshLocalDataCalls == 0);
}

TEST_CASE("issue #1081: ShouldKickInitialSync — failure-backoff window gates the per-frame kick site") {
    const auto now = std::chrono::steady_clock::now();

    // Fresh context: never kicked, no retry window → kick.
    CHECK(smatchet::ShouldKickInitialSync(now, false, std::chrono::steady_clock::time_point{}));
    // Latched: already kicked → never re-kick regardless of the window.
    CHECK_FALSE(smatchet::ShouldKickInitialSync(now, true, std::chrono::steady_clock::time_point{}));
    CHECK_FALSE(smatchet::ShouldKickInitialSync(now, true, now + std::chrono::seconds(30)));
    // Failed session re-armed the latch AND opened a 30 s window: inside → no kick (this is
    // the storm fix — pre-fix the re-arm alone allowed a re-kick EVERY FRAME on fast-fail).
    CHECK_FALSE(smatchet::ShouldKickInitialSync(now, false, now + std::chrono::seconds(30)));
    // Window elapsed → retry allowed.
    CHECK(smatchet::ShouldKickInitialSync(now, false, now - std::chrono::seconds(1)));
    CHECK(smatchet::ShouldKickInitialSync(now, false, now)); // boundary: now >= retryAfter
}

TEST_CASE("issue #1081: PaneSyncKickStillCurrent — stale-generation kick is dropped (no backend re-swap)") {
    // Current: context alive, generation unmoved → the kick applies.
    CHECK(smatchet::PaneSyncKickStillCurrent(true, 7u, 7u));
    // Backend swapped (generation moved) while the kick's worker ran → drop. This is the
    // stale-cfgCopy no-reswap branch: applying the pre-switch Jira cfg would call
    // SyncPaneWithBackend → SwapBackendIfTrackerChanged → factory Create("Jira") and swap
    // the just-switched GitHub backend BACK.
    CHECK_FALSE(smatchet::PaneSyncKickStillCurrent(true, 7u, 8u));
    // Context retired mid-kick → drop (generation values are unrelated counters then).
    CHECK_FALSE(smatchet::PaneSyncKickStillCurrent(false, 7u, 7u));
    CHECK_FALSE(smatchet::PaneSyncKickStillCurrent(false, 0u, 0u));
}
