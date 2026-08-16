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
// Per-case isolation: every TEST_CASE constructs its own FakeTicketSyncDeps + FakeSyncCache +
// fresh TicketSyncService. No statics, no shared world state, no order dependencies.

#include "../support/FakeTicketSyncDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/ScriptedTrackerBackendFactory.h"

#include "AppController.h" // for TrackerIssueFetchPack
#include "CachedTicketTypes.h"
#include "ITicketSyncDeps.h"
#include "TicketSyncService.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using smatchet_tests::FakeTicketSyncDeps;
using smatchet_tests::FakeTrackerClient;
using smatchet_tests::JiraFakeTrackerFixture;
using smatchet_tests::ScriptedTrackerBackendFactory;

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
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1", "first"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2", "second"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-3", "third"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ABC-1", "first-refreshed"));
    pack.FullSyncCompleted = false; // partial fetch — must not delete unmentioned rows
    svc.ApplyIssueFetchPack(pack);

    std::vector<CachedTicket> all = deps.CacheImpl->GetAllTickets("Jira");
    CHECK(all.size() == 3);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "first-refreshed");
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldValues["summary"] == "second");
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-3", got));
    CHECK(got.fieldValues["summary"] == "third");

    CHECK(deps.PendingLuaWindowBumpImpl == true);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack full-sync deletes stale rows outside the fetched set" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-3"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-4"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ABC-1", "kept"));
    pack.Tickets.push_back(MakeTicket("ABC-2", "kept"));
    pack.FullSyncCompleted = true;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");

    CachedTicket got;
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("Jira", "ABC-3", got));
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("Jira", "ABC-4", got));

    CHECK(deps.PendingLuaWindowBumpImpl == true);
    CHECK(deps.DeferredLiveNotifyCalls == 1);
}

TEST_CASE("TicketSyncService::FilterStaleIdsRetainedElsewhere drops sibling-pane ids and preserves order") {
    std::vector<std::string> stale;
    stale.push_back("ABC-1");
    stale.push_back("ABC-2");
    stale.push_back("ABC-3");
    stale.push_back("ABC-4");

    SUBCASE("empty retention set is a pass-through") {
        std::vector<std::string> kept =
            TicketSyncService::FilterStaleIdsRetainedElsewhere(stale, std::vector<std::string>());
        CHECK(kept == stale);
    }

    SUBCASE("ids held by another pane survive, order preserved") {
        std::vector<std::string> retained;
        retained.push_back("ABC-3");
        retained.push_back("ABC-1");
        retained.push_back("ZZZ-9"); // not stale here — an unrelated id must not disturb the result
        std::vector<std::string> kept = TicketSyncService::FilterStaleIdsRetainedElsewhere(stale, retained);
        REQUIRE(kept.size() == 2);
        CHECK(kept[0] == "ABC-2");
        CHECK(kept[1] == "ABC-4");
    }

    SUBCASE("every stale id retained elsewhere yields an empty deletion set") {
        std::vector<std::string> kept = TicketSyncService::FilterStaleIdsRetainedElsewhere(stale, stale);
        CHECK(kept.empty());
    }
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack full-sync keeps rows a sibling pane is displaying" *
          doctest::test_suite("[high-risk]")) {
    // Multi-grid collision (ADR-0018 decision 4): panes share ONE backend-keyed cache namespace
    // but each syncs its own query, so "cached row my query did not return" over-claims. Rows a
    // sibling pane is displaying must survive this pane's full-sync sweep.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("SIB-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("SIB-2"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("GONE-1"));
    deps.RetainedByOtherContextsImpl.push_back("SIB-1");
    deps.RetainedByOtherContextsImpl.push_back("SIB-2");
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("ABC-1", "kept"));
    pack.Tickets.push_back(MakeTicket("ABC-2", "kept"));
    pack.FullSyncCompleted = true;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 4);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(ids[2] == "SIB-1");
    CHECK(ids[3] == "SIB-2");

    // A row nobody holds and this query did not return is still genuinely stale.
    CachedTicket got;
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("Jira", "GONE-1", got));
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack full-sync empty pack rejects stale-deletion and preserves cache" *
          doctest::test_suite("[high-risk]")) {
    // Guard at TicketSyncService.cpp:82-86 — a full-sync that returns zero tickets cannot
    // prove the cache is stale, so the stale-deletion branch is skipped. A 200-with-empty-
    // body network glitch (or a transient backend bug) must NOT silently wipe every cached
    // row. Genuinely-empty projects re-converge on the next non-empty fetch.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();
    pack.FullSyncCompleted = true;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(deps.DeferredLiveNotifyCalls == 1);
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack partial empty pack is a no-op for cache state") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(deps.DeferredLiveNotifyCalls == 1);
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

// The exact partial-error combo the empty-fetch guard must survive without touching the cache:
// a non-empty FetchError + FullSyncCompleted=false + empty Tickets (backlog test/2026-05-17).
// Distinct from the clean partial-empty case above: because FetchError is non-empty the success
// branch is skipped, so NO deferred success-notify fires (==0, not ==1) — that's the coverage gap.
TEST_CASE(
    "TicketSyncService::ApplyIssueFetchPack transient error + empty partial fetch preserves cache, no success-notify") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();           // empty fresh set
    pack.FullSyncCompleted = false; // partial — must never delete unmentioned rows
    pack.FetchError = "connection reset by peer";
    pack.FetchErrorTransient = true; // transport-style: routes to the TransportDown path
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2); // cache fully preserved — no prune on a partial error
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(deps.DeferredLiveNotifyCalls == 0); // error path: NOT a success
    CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::TransportDown);
    CHECK(deps.LastTrackerTicketSyncWarningTransient == true);
    CHECK_FALSE(deps.LastTrackerTicketSyncWarning.empty());
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack non-transient error + empty partial fetch preserves cache, no "
          "transport-down") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("ABC-2"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.clear();
    pack.FullSyncCompleted = false;
    pack.FetchError = "HTTP 500 from backend";
    pack.FetchErrorTransient = false; // non-transport error: neither success nor TransportDown
    svc.ApplyIssueFetchPack(pack);

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2); // cache preserved
    CHECK(ids[0] == "ABC-1");
    CHECK(ids[1] == "ABC-2");
    CHECK(deps.DeferredLiveNotifyCalls == 0);                                               // still not a success
    CHECK(deps.LastConnectivityState != ITicketSyncDeps::ConnectivityState::TransportDown); // not routed transport-down
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack replaces existing row's field snapshot in-place") {
    FakeTicketSyncDeps deps;
    CachedTicket initial = MakeTicket("ABC-1", "old summary");
    initial.fieldValues["priority"] = "Low";
    initial.fieldValues["assignee"] = "alice";
    deps.CacheImpl->SaveTicket("Jira", initial);
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
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "new summary");
    CHECK(got.fieldValues["priority"] == "High");
    CHECK(got.fieldValues.count("assignee") == 0);
    CHECK(got.fieldValues.count("status") == 0);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack inserts brand-new row when id absent from cache") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("EXISTING-1"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(MakeTicket("NEW-1", "fresh"));
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "NEW-1", got));
    CHECK(got.fieldValues["summary"] == "fresh");
    CHECK(got.fieldValues["status"] == "Open");
    // Existing row untouched.
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "EXISTING-1", got));
    CHECK(got.fieldValues["summary"] == "summary");

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
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
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "STREAM-1", got));
    CHECK(got.fieldValues["summary"] == "alpha");
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "STREAM-2", got));
    CHECK(got.fieldValues["summary"] == "beta");

    // Cancel + join before fixture destruction — defensive even though the worker is already
    // finished, mirrors what ~AppController does in production.
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService streamed seam prefers the backend's structured kind over the text sniff" *
          doctest::test_suite("[high-risk]")) {
    // N12 item 12: the fake scripts a fetch failure whose TEXT the heuristic would NOT call
    // transport ("backend rejected the streamed query") but whose structured kind is Transport.
    // The worker-side seam must classify transient from summary.Error — flipping connectivity
    // to TransportDown — proving the structured kind is authoritative when a backend sets it.
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesError(TrackerErrorTransport("backend rejected the streamed query"));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    const bool drained = SpinUntil(svc, [&]() {
        return !svc.IsActive() && deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::TransportDown;
    });
    REQUIRE(drained);
    CHECK(deps.LastTrackerTicketSyncWarningTransient);
    CHECK(deps.LastTrackerTicketSyncWarning.find("backend rejected the streamed query") != std::string::npos);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService::SyncWithBackend routes user-facing toasts through NotifySyncStatus (UI-free)") {
    // sync-imgui-coupling decouple (debt 2026-06-07): TicketSyncService no longer pokes the
    // ImGui SmatchetToastManager directly — every user-facing toast now flows through
    // ITicketSyncDeps::NotifySyncStatus, which the fake records. Assert the happy-path sequence:
    // a "Syncing" Info toast fires synchronously at session start, then a "Sync Complete" Success
    // toast fires once the worker drains. No ToastType / imgui.h is reachable from this TU.
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("TOAST-1", "alpha"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake"; // matches FakeTrackerClient::GetTrackerType — no backend swap
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    // The "Syncing" Info toast is emitted synchronously inside StartStreamingSync.
    REQUIRE_FALSE(deps.SyncNotifications.empty());
    CHECK(deps.SyncNotifications.front().Title == "Syncing");
    CHECK(deps.SyncNotifications.front().Level == ITicketSyncDeps::SyncNotifyLevel::Info);

    const bool drained = SpinUntil(svc, [&]() {
        for (const auto& n : deps.SyncNotifications) {
            if (n.Title == "Sync Complete") {
                return true;
            }
        }
        return false;
    });
    REQUIRE(drained);

    bool sawComplete = false;
    for (const auto& n : deps.SyncNotifications) {
        if (n.Title == "Sync Complete") {
            sawComplete = true;
            CHECK(n.Level == ITicketSyncDeps::SyncNotifyLevel::Success);
        }
    }
    CHECK(sawComplete);

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
    pack.FetchError = "HTTP 400: invalid JQL"; // non-transport; the pack composer classifies → false
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ERR-1", got));
    CHECK(got.fieldValues["summary"] == "summary");

    CHECK(deps.PushOutageCalls == 0);
    CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::Unknown);
    CHECK(deps.DeferredLiveNotifyCalls == 0);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());
    CHECK(deps.PendingLuaWindowBumpImpl == true);
}

TEST_CASE("TicketSyncService::ApplyIssueFetchPack branches on the pack's transport flag, not the error text" *
          doctest::test_suite("[high-risk]")) {
    // N12 slice 1 regression witness: transport-ness is decided ONCE where the pack is composed
    // and travels as FetchErrorTransient. A transport-shaped error TEXT with the flag left false
    // must NOT flip connectivity — proving this consumer no longer re-classifies the string —
    // and the flag set true takes the transport path, tagging the warning it emits.
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);

    {
        TrackerIssueFetchPack pack;
        pack.FetchError = "Connection timeout after 30000 ms"; // transport-shaped TEXT...
        pack.FetchErrorTransient = false;                      // ...but the composer said no
        svc.ApplyIssueFetchPack(pack);
        CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::Unknown);
        CHECK(deps.PushOutageCalls == 0);
        CHECK(deps.LastTrackerTicketSyncWarning.empty());
    }
    {
        TrackerIssueFetchPack pack;
        pack.FetchError = "backend said no";
        pack.FetchErrorTransient = true;
        svc.ApplyIssueFetchPack(pack);
        CHECK(deps.LastConnectivityState == ITicketSyncDeps::ConnectivityState::TransportDown);
        CHECK(deps.PushOutageCalls == 1);
        CHECK(deps.LastTrackerTicketSyncWarning.find("live refresh did not complete") != std::string::npos);
        CHECK(deps.LastTrackerTicketSyncWarningTransient);
    }
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
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("IDLE-1"));
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
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "IDLE-1", got));
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

    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == "BATCH-0");
    CHECK(ids[1] == "BATCH-1");
    CHECK(ids[2] == "BATCH-2");

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "BATCH-0", got));
    CHECK(got.fieldValues["summary"] == "v0");
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "BATCH-2", got));
    CHECK(got.fieldValues["summary"] == "v2");

    CHECK(deps.DeferredLiveNotifyCalls == 3);
    CHECK(deps.PushOutageCalls == 0);
    CHECK_FALSE(svc.IsActive());
}

// ---------------------------------------------------------------------------
// Backend swap matrix — Slice 0 Workstream 1 of docs/plans/multi-grid-tabs.md
// (characterize-existing regression net). Pins SwapBackendIfTrackerChanged's
// CURRENT single-active-backend semantics before the multi-pane refactor:
// the swap happens synchronously inside SyncWithBackend -> StartStreamingSync
// (before the worker spawns), so the new backend kind is observable the moment
// SyncWithBackend returns.
// ---------------------------------------------------------------------------

namespace {

/// FakeTicketSyncDeps wired with a ScriptedTrackerBackendFactory so that
/// BackendFactory()->Create(kind) returns clients whose GetTrackerType()
/// reports the requested kind ("Jira" via the fixture, "Plane"/"GitHub" as
/// plain FakeTrackerClient(kind)). The stock FakeTrackerBackendFactory
/// ignores the kind argument, which would make swap assertions vacuous.
void UseScriptedFactory(FakeTicketSyncDeps& deps) {
    deps.Factory.reset(new ScriptedTrackerBackendFactory(JiraFakeTrackerFixture::LoadFromString("{}")));
}

std::string CurrentBackendType(FakeTicketSyncDeps& deps) {
    return deps.BackendConnectivity() ? deps.BackendConnectivity()->GetTrackerType() : std::string();
}

} // namespace

TEST_CASE("TicketSyncService swap matrix: Jira -> Plane -> GitHub -> Jira creates the right client per kind" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    TicketSyncService svc(deps);

    // Initial backend is the stock FakeTrackerClient ("fake").
    CHECK(CurrentBackendType(deps) == "fake");

    TrackerConfig cfg;
    ViewsStore views;

    cfg.TrackerType = "Plane";
    svc.SyncWithBackend(&cfg, &views);
    CHECK(CurrentBackendType(deps) == "Plane");
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    cfg.TrackerType = "GitHub";
    svc.SyncWithBackend(&cfg, &views);
    CHECK(CurrentBackendType(deps) == "GitHub");
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    cfg.TrackerType = "Jira";
    svc.SyncWithBackend(&cfg, &views);
    CHECK(CurrentBackendType(deps) == "Jira");
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    // Mixed/lowercase kind still routes correctly (GitHubClient reports "github").
    cfg.TrackerType = "plane";
    svc.SyncWithBackend(&cfg, &views);
    CHECK(CurrentBackendType(deps) == "Plane");

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService swap matrix: empty tracker type defaults to Jira") {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = ""; // empty -> treated as "Jira"
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    CHECK(CurrentBackendType(deps) == "Jira");

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService same-kind config change performs no backend swap and keeps active tickets") {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    deps.SetBackend(std::unique_ptr<ITrackerBackend>(new FakeTrackerClient("Jira")));
    deps.ActiveTicketsImpl.push_back(MakeTicket("KEEP-1"));
    TicketSyncService svc(deps);

    ITrackerConnectivity* before = deps.BackendConnectivity();
    REQUIRE(before != nullptr);

    TrackerConfig cfg;
    cfg.TrackerType = "Jira"; // same kind, different JQL — config change without kind change
    cfg.JqlQuery = "project = OTHER";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    // Identical connectivity pointer == the backend instance was NOT recreated.
    CHECK(deps.BackendConnectivity() == before);
    // In-memory active tickets survive a same-kind sync start.
    CHECK(deps.ActiveTicketsImpl.size() == 1);
    CHECK(deps.ActiveTicketsRevisionImpl == 0);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService kind change clears in-memory active tickets and publishes an empty snapshot" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    deps.SetBackend(std::unique_ptr<ITrackerBackend>(new FakeTrackerClient("Jira")));
    deps.ActiveTicketsImpl.push_back(MakeTicket("OLD-1"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("OLD-2"));
    // Cache rows must SURVIVE the swap (cache hydrate repopulates on switch-back).
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("OLD-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("OLD-2"));
    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    CHECK(CurrentBackendType(deps) == "Plane");
    CHECK(deps.ActiveTicketsImpl.empty());
    REQUIRE(deps.ActiveTicketsPublishedImpl != nullptr);
    CHECK(deps.ActiveTicketsPublishedImpl->empty());
    CHECK(deps.ActiveTicketsRevisionImpl == 1);
    // Every ActiveTickets-mutating path flips the Lua window bump (invariant).
    CHECK(deps.PendingLuaWindowBumpImpl == true);
    // SQLite rows are untouched by the in-memory clear.
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "OLD-1", got));
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "OLD-2", got));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService::SyncWithBackend while busy defers via pendingConfig_ and supersedes the old session" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    TicketSyncService svc(deps);

    TrackerConfig jiraCfg;
    jiraCfg.TrackerType = "Jira";
    ViewsStore views;
    svc.SyncWithBackend(&jiraCfg, &views);
    // Session is running until a TickStreamingApply finalizes it — we have not ticked,
    // so the service is deterministically busy regardless of worker speed.
    REQUIRE(svc.IsActive());
    CHECK(CurrentBackendType(deps) == "Jira");

    // Second request while busy: deferred (Cancelled/Superseded set, pendingConfig_
    // captured) — the backend must NOT swap yet.
    TrackerConfig planeCfg;
    planeCfg.TrackerType = "Plane";
    svc.SyncWithBackend(&planeCfg, &views);
    CHECK(CurrentBackendType(deps) == "Jira"); // swap deferred with the request
    CHECK(svc.IsActive());

    // Draining ticks discards the superseded session and starts the pending request
    // with the SECOND config — the backend kind flips to Plane.
    REQUIRE(SpinUntil(svc, [&]() { return CurrentBackendType(deps) == "Plane"; }));
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(CurrentBackendType(deps) == "Plane");

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService busy-defer keeps only the LAST pending request (latest config wins)") {
    FakeTicketSyncDeps deps;
    UseScriptedFactory(deps);
    TicketSyncService svc(deps);

    TrackerConfig jiraCfg;
    jiraCfg.TrackerType = "Jira";
    ViewsStore views;
    svc.SyncWithBackend(&jiraCfg, &views);
    REQUIRE(svc.IsActive());

    // Two deferred requests in a row: pendingConfig_ is overwritten — only the
    // GitHub one materializes once the busy session is discarded.
    TrackerConfig planeCfg;
    planeCfg.TrackerType = "Plane";
    svc.SyncWithBackend(&planeCfg, &views);
    TrackerConfig githubCfg;
    githubCfg.TrackerType = "GitHub";
    svc.SyncWithBackend(&githubCfg, &views);
    CHECK(CurrentBackendType(deps) == "Jira"); // still no swap before drain

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    // The deferred restart applied the LAST config; "Plane" was never created.
    CHECK(CurrentBackendType(deps) == "GitHub");

    svc.CancelAndJoinActiveStreamingSync();
}
