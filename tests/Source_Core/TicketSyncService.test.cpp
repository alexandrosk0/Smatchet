// TicketSyncService runtime tests — exercise the streaming-sync apply paths through the
// ITicketSyncDeps interface bundle introduced by PR D. Tests drive the service against a
// FakeTicketSyncDeps fixture (header-only, in-memory SQLite + FakeTrackerClient + plain-member
// connectivity state) so no AppController, ImGui, cpr, or HTTP surface is needed at runtime.
//
// Coverage matrix:
//   * ApplyIssueFetchPack — partial vs full-sync stale-deletion, empty-pack handling, insert /
//     update of cache rows, transport-error vs non-transport-error connectivity-banner routing.
//   * TickStreamingApply — idle no-op path, end-to-end happy path via SyncWithBackend.
//   * Worker-result drain — RequestDeferredLiveTrackerBackendSuccessNotify + Lua window-bump
//     coalescing.
//   * Re-entrancy + concurrent-read smoke — repeated apply / tick from a single thread.
//
// Per-case isolation: every TEST_CASE constructs its own FakeTicketSyncDeps + :memory: cache +
// fresh TicketSyncService. No statics, no shared world state, no order dependencies.

#include "../support/FakeTicketSyncDeps.h"
#include "../support/FakeTrackerClient.h"

#include "AppController.h" // for TrackerIssueFetchPack
#include "CachedTicketTypes.h"
#include "ITicketSyncDeps.h"
#include "LocalCacheManager.h"
#include "TicketSyncService.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using smatchet_tests::FakeTicketSyncDeps;
using smatchet_tests::FakeTrackerClient;

namespace {

CachedTicket MakeTicket(const std::string& id, const std::string& summary = "summary") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    t.fieldValues["status"] = "Open";
    return t;
}

// Spin until predicate or hard cap. Used only for the SyncWithBackend end-to-end case
// where the worker thread completes near-instantly against the fake backend.
template <typename Pred> bool SpinUntil(TicketSyncService& svc, Pred pred, int maxTicks = 200) {
    for (int i = 0; i < maxTicks; ++i) {
        svc.TickStreamingApply();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("TicketSyncService::ApplyIssueFetchPack partial-fetch leaves rows outside the fetched set untouched") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-1", "first"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-2", "second"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-3", "third"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ABC-1", "first-refreshed"));
    pack.FullSyncCompleted = false; // partial fetch — must not delete unmentioned rows
    svc.ApplyIssueFetchPack(pack);

    std::vector<CachedTicket> all = deps.CacheImpl->GetAllTickets();
    CHECK(all.size() == 3);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("ABC-1", got));
    CHECK(got.fieldValues["summary"] == "first-refreshed");
    REQUIRE(deps.CacheImpl->TryGetTicket("ABC-2", got));
    CHECK(got.fieldValues["summary"] == "second");
    REQUIRE(deps.CacheImpl->TryGetTicket("ABC-3", got));
    CHECK(got.fieldValues["summary"] == "third");

    CHECK(deps.PendingLuaWindowBumpImpl == true);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack full-sync deletes stale rows outside the fetched set" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-2"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-3"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-4"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ABC-1", "kept"));
    pack.Tickets.push_back(MakeTicket("ABC-2", "kept"));
    pack.FullSyncCompleted = true;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds();
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");

    CachedTicket got;
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("ABC-3", got));
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("ABC-4", got));

    CHECK(deps.PendingLuaWindowBumpImpl == true);
    CHECK(deps.DeferredLiveNotifyCalls == 1);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack full-sync empty pack documents current delete-all behavior" *
          doctest::test_suite("[high-risk]")) {
    // Spec docs/design/test-suite-expansion-completion.md § PR F lists this case as
    //   "empty fetch in full-sync mode → rejects, does NOT delete"
    // but the production code at TicketSyncService.cpp lines 81-97 unconditionally deletes
    // every row not in `keepIds` — an empty fetch makes `keepIds` empty, so all rows go.
    // This test captures the CURRENT behavior; mismatch flagged in Self-improvement so the
    // orchestrator can route the safeguard to `offline-sync` as a production-side fix.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();
    pack.FullSyncCompleted = true;
    svc.ApplyIssueFetchPack(pack);

    // Current behavior: all rows deleted. If a future PR adds the safeguard described in the
    // spec, flip these assertions to `CHECK(ids.size() == 2);` and rewrite the rationale.
    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds();
    CHECK(ids.empty());
    CHECK(deps.DeferredLiveNotifyCalls == 1);
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack partial empty pack is a no-op for cache state") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket(MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds();
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(deps.DeferredLiveNotifyCalls == 1);
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack replaces existing row's field snapshot in-place") {
    FakeTicketSyncDeps deps;
    CachedTicket initial = MakeTicket("ABC-1", "old summary");
    initial.fieldValues["priority"] = "Low";
    initial.fieldValues["assignee"] = "alice";
    deps.CacheImpl->SaveTicket(initial);
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    CachedTicket refreshed;
    refreshed.id = "ABC-1";
    refreshed.fieldValues["summary"] = "new summary";
    refreshed.fieldValues["priority"] = "High";
    // assignee intentionally omitted — SaveTicket replaces full snapshot per LocalCacheManager
    // contract; the assignee field should disappear.
    pack.Tickets.push_back(refreshed);
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("ABC-1", got));
    CHECK(got.fieldValues["summary"] == "new summary");
    CHECK(got.fieldValues["priority"] == "High");
    CHECK(got.fieldValues.count("assignee") == 0);
    CHECK(got.fieldValues.count("status") == 0);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack inserts brand-new row when id absent from cache") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("EXISTING-1"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("NEW-1", "fresh"));
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("NEW-1", got));
    CHECK(got.fieldValues["summary"] == "fresh");
    CHECK(got.fieldValues["status"] == "Open");
    // Existing row untouched.
    REQUIRE(deps.CacheImpl->TryGetTicket("EXISTING-1", got));
    CHECK(got.fieldValues["summary"] == "summary");

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds();
    CHECK(ids.size() == 2);
}

TEST_CASE("TicketSyncService::TickStreamingApply with no active session is a side-effect-free no-op") {
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);

    CHECK_FALSE(svc.IsActive());

    svc.TickStreamingApply();
    svc.TickStreamingApply();
    svc.TickStreamingApply();

    CHECK_FALSE(svc.IsActive());
    CHECK(deps.LastTrackerTicketSyncWarning.empty());
    CHECK(deps.DeferredLiveNotifyCalls == 0);
    CHECK(deps.PushOutageCalls == 0);
    CHECK(deps.WarmIssueTypeEditMetaCalls == 0);
    CHECK(deps.NotifyLuaCalls == 0);
    CHECK(deps.ActiveTicketsImpl.empty());
}

TEST_CASE("TicketSyncService::SyncWithBackend end-to-end populates cache + active-tickets via worker drain") {
    FakeTicketSyncDeps deps;
    // Script the fake backend to return two tickets on the streaming fetch.
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("STREAM-1", "alpha"));
    scripted.push_back(MakeTicket("STREAM-2", "beta"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake"; // matches FakeTrackerClient::GetTrackerType — no backend swap
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    const bool drained = SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 2; });

    REQUIRE(drained);
    CHECK_FALSE(svc.IsActive());
    CHECK(deps.ActiveTicketsImpl.size() == 2);
    CHECK(deps.DeferredLiveNotifyCalls >= 1);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("STREAM-1", got));
    CHECK(got.fieldValues["summary"] == "alpha");
    REQUIRE(deps.CacheImpl->TryGetTicket("STREAM-2", got));
    CHECK(got.fieldValues["summary"] == "beta");

    // Cancel + join before fixture destruction — defensive even though the worker is already
    // finished, mirrors what ~AppController does in production.
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack with non-transport FetchError still applies tickets without "
          "flipping connectivity state" *
          doctest::test_suite("[high-risk]")) {
    // Mirrors the "error during apply" plan case: a non-transport-style fetch error (e.g. an
    // HTTP 4xx server-side validation rejection) is NOT a transport outage — the apply should
    // still persist the tickets that did come back and must NOT push offline-replay timers
    // forward or flip the connectivity state to TransportDown. The deferred-live-notify is
    // also skipped because fetchError is non-empty.
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ERR-1"));
    pack.FullSyncCompleted = false;
    pack.FetchError = "HTTP 400: invalid JQL"; // non-transport; IsTrackerTransportErrorText → false
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("ERR-1", got));
    CHECK(got.fieldValues["summary"] == "summary");

    CHECK(deps.PushOutageCalls == 0);
    CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::Unknown);
    CHECK(deps.DeferredLiveNotifyCalls == 0);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack success path posts deferred-live-notify exactly once") {
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("OK-1"));
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CHECK(deps.DeferredLiveNotifyCalls == 1);
    CHECK(deps.PushOutageCalls == 0);
    CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::Unknown);
    CHECK(deps.PendingLuaWindowBumpImpl == true);

    // Second apply pack still successful — counter increments, no spurious outage state.
    TrackerIssueFetchPack pack2;
    pack2.Tickets.push_back(MakeTicket("OK-2"));
    pack2.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack2);
    CHECK(deps.DeferredLiveNotifyCalls == 2);
    CHECK(deps.PushOutageCalls == 0);
}

TEST_CASE("TicketSyncService::TickStreamingApply repeated idle calls produce no spurious state mutations") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket(MakeTicket("IDLE-1"));
    TicketSyncService svc(deps);

    for (int i = 0; i < 20; ++i) {
        svc.TickStreamingApply();
    }

    CHECK_FALSE(svc.IsActive());
    CHECK(deps.DeferredLiveNotifyCalls == 0);
    CHECK(deps.PushOutageCalls == 0);
    CHECK(deps.WarmIssueTypeEditMetaCalls == 0);
    CHECK(deps.NotifyLuaCalls == 0);
    CHECK_FALSE(deps.PendingLuaWindowBumpImpl);
    // Cache row from setup must survive.
    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("IDLE-1", got));
    CHECK(got.fieldValues["summary"] == "summary");
}

TEST_CASE("TicketSyncService back-to-back ApplyIssueFetchPack invocations stay consistent across N ticks") {
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);

    // Three sequential partial-fetch packs simulating a streaming sync that the UI thread
    // is draining one batch at a time. Cache + counters must stay coherent.
    for (int i = 0; i < 3; ++i) {
        TrackerIssueFetchPack pack;
        CachedTicket t = MakeTicket("BATCH-" + std::to_string(i), "v" + std::to_string(i));
        pack.Tickets.push_back(t);
        pack.FullSyncCompleted = false;
        svc.ApplyIssueFetchPack(pack);
        // Interleave idle ticks — should not mutate state since no streaming session is active.
        svc.TickStreamingApply();
    }

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds();
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == "BATCH-0");
    CHECK(ids[1] == "BATCH-1");
    CHECK(ids[2] == "BATCH-2");

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("BATCH-0", got));
    CHECK(got.fieldValues["summary"] == "v0");
    REQUIRE(deps.CacheImpl->TryGetTicket("BATCH-2", got));
    CHECK(got.fieldValues["summary"] == "v2");

    CHECK(deps.DeferredLiveNotifyCalls == 3);
    CHECK(deps.PushOutageCalls == 0);
    CHECK_FALSE(svc.IsActive());
}
