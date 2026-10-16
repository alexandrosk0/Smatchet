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
#include "../support/TestEnvGuard.h"

#include "AppController.h" // for TrackerIssueFetchPack
#include "CachedTicketTypes.h"
#include "ITicketSyncDeps.h"
#include "TicketSyncService.h"
#include "Sync/ChangeMonitorRosterProbe.h"
#include "Sync/WalkOwnedKeysPure.h"
#include "Sync/DeltaSyncPure.h"
#include "Sync/RemoteChangeInvalidationPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

// A comment thread / tooltip blob this session holds (a lazy fetch, or a sync of a backend whose
// search payload carries bodies) must survive a sync row that brings none, while the comment count
// is unchanged — the comments modal shows it offline (Quality Pillar 6).
namespace {

CachedTicket WithComments(CachedTicket t, const std::string& count, const std::string& blob,
                          const std::string& thread) {
    t.fieldValues["comments"] = count;
    if (!blob.empty()) {
        t.fieldValues["comment"] = blob;
    }
    if (!thread.empty()) {
        t.fieldRichValues[kCommentThreadRichKey] = thread;
    }
    return t;
}

} // namespace

TEST_CASE("TicketSyncService::ApplyIssueFetchPack carries the saved comment thread over an unchanged count") {
    FakeTicketSyncDeps deps;
    deps.ActiveTicketsImpl.push_back(WithComments(MakeTicket("ABC-1"), "2", "blob-1", "[\"thread-1\"]"));
    deps.ActiveTicketsImpl.push_back(WithComments(MakeTicket("ABC-2"), "1", "blob-2", "[\"thread-2\"]"));
    TicketSyncService svc(deps);

    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(WithComments(MakeTicket("ABC-1", "renamed"), "2", "", ""));
    pack.Tickets.push_back(WithComments(MakeTicket("ABC-2"), "2", "", "")); // a new comment arrived
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "renamed");
    CHECK(got.GetFieldRichValue(kCommentThreadRichKey) == "[\"thread-1\"]");
    CHECK(got.GetFieldValue("comment") == "blob-1");
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldRichValues.count(kCommentThreadRichKey) == 0);
    CHECK(got.GetFieldValue("comment").empty());
}

TEST_CASE("TicketSyncService streaming apply carries the saved comment thread into cache and memory") {
    FakeTicketSyncDeps deps;
    deps.ActiveTicketsImpl.push_back(WithComments(MakeTicket("STREAM-1"), "1", "blob-1", "[\"thread-1\"]"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(WithComments(MakeTicket("STREAM-1", "alpha"), "1", "", ""));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    CachedTicket got;
    const bool drained = SpinUntil(svc, [&]() {
        return !svc.IsActive() && deps.CacheImpl->TryGetTicket("Jira", "STREAM-1", got) &&
               got.fieldValues["summary"] == "alpha";
    });
    REQUIRE(drained);
    CHECK(got.GetFieldRichValue(kCommentThreadRichKey) == "[\"thread-1\"]");
    CHECK(got.GetFieldValue("comment") == "blob-1");
    {
        std::lock_guard<std::mutex> lock(deps.ActiveTicketsMutexImpl);
        const auto it = std::find_if(deps.ActiveTicketsImpl.begin(), deps.ActiveTicketsImpl.end(),
                                     [](const CachedTicket& t) { return t.id == "STREAM-1"; });
        REQUIRE(it != deps.ActiveTicketsImpl.end());
        CHECK(it->GetFieldRichValue(kCommentThreadRichKey) == "[\"thread-1\"]");
    }
    svc.CancelAndJoinActiveStreamingSync();
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

TEST_CASE("TicketSyncService destroyed right after its session finalizes joins the worker (no std::terminate)" *
          doctest::test_suite("[high-risk]")) {
    // Crash regression: IsActive() reads false from the finalize tick onward, but the finished
    // worker's std::thread is only joined on the NEXT tick (StartPendingSyncIfIdle).
    // AppController::retireExpiredHiddenContexts_ trusts IsActive() and resets the service in
    // that window; without a joining destructor the joinable std::thread member called
    // std::terminate and the app aborted. Deliberately NO CancelAndJoinActiveStreamingSync()
    // here — the destructor alone must make teardown safe. A regression aborts this binary.
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    // A failed fetch, matching the crash log (GitHub HTTP 0 → session finished with err).
    fake->SetFetchIssuesError(TrackerErrorTransport("HTTP 0"));

    std::unique_ptr<TicketSyncService> svc = std::make_unique<TicketSyncService>(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc->SyncWithBackend(&cfg, &views);

    // Stop on the FIRST tick that reports idle — the finalize tick — so the worker thread is
    // finished but not yet joined, exactly the state retirement observed.
    const bool idle = SpinUntil(*svc, [&]() { return !svc->IsActive(); });
    REQUIRE(idle);
    // Pane retirement: ctx.ticketSync_.reset(). The assertion is that this returns at all —
    // a regression calls std::terminate here and doctest reports the case as CRASHED.
    svc.reset();
}

TEST_CASE("TicketSyncService::CancelAndJoinAll flags every busy pane before joining any" *
          doctest::test_suite("[high-risk]")) {
    // Shutdown-latency regression: ~AppController used to cancel+join only the focused pane; every
    // other busy pane was then joined by ~TicketSyncService during gridContexts_ destruction, one
    // after another. A worker that is not yet flagged keeps paging while an earlier pane's join
    // waits, so each pane added up to one in-flight request timeout. CancelAndJoinAll must flag ALL
    // services before joining ANY, so every worker stops at its current page boundary.
    //
    // The primary assertion is the page count, not wall-clock: with the two-pass cancel neither
    // worker may start another page after CancelAndJoinAll begins. (A one-pass loop leaves the
    // second worker unflagged across its in-flight page boundary, where it starts one more.)
    // Counts are snapshotted just before the cancel, so worker start-up skew cannot fail the case.
    // The elapsed-time bound is a loose sanity check only.
    constexpr std::chrono::milliseconds kPageDelay(300);
    constexpr int kPages = 50; // ~15 s if never cancelled

    FakeTicketSyncDeps depsA;
    FakeTicketSyncDeps depsB;
    auto* fakeA = static_cast<FakeTrackerClient*>(depsA.BackendImpl.get());
    auto* fakeB = static_cast<FakeTrackerClient*>(depsB.BackendImpl.get());
    fakeA->SetSlowStreamedPages(kPages, kPageDelay);
    fakeB->SetSlowStreamedPages(kPages, kPageDelay);

    TicketSyncService svcA(depsA);
    TicketSyncService svcB(depsB);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svcA.SyncWithBackend(&cfg, &views);
    svcB.SyncWithBackend(&cfg, &views);

    // Wait until both workers have their first request in flight.
    bool bothInFlight = false;
    for (int i = 0; i < 1000 && !bothInFlight; ++i) {
        bothInFlight = fakeA->SlowStreamedPagesStarted() >= 1 && fakeB->SlowStreamedPagesStarted() >= 1;
        if (!bothInFlight) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(bothInFlight);
    REQUIRE(svcA.IsActive());
    REQUIRE(svcB.IsActive());

    std::vector<TicketSyncService*> services;
    services.push_back(&svcA);
    services.push_back(&svcB);
    services.push_back(&svcA); // duplicate (focused pane is also a live entry) is tolerated
    services.push_back(nullptr);
    const int pagesBeforeA = fakeA->SlowStreamedPagesStarted();
    const int pagesBeforeB = fakeB->SlowStreamedPagesStarted();
    const auto start = std::chrono::steady_clock::now();
    TicketSyncService::CancelAndJoinAll(services);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    CHECK(fakeA->SlowStreamedPagesStarted() == pagesBeforeA);
    CHECK(fakeB->SlowStreamedPagesStarted() == pagesBeforeB);
    CHECK_FALSE(svcA.IsActive());
    CHECK_FALSE(svcB.IsActive());
    // Both in-flight pages drain concurrently: about one page delay in total. Serial joins would
    // cost about two; a never-cancelled worker would cost kPages. Generous slack for CI.
    CHECK(elapsed < kPageDelay * 4);
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

// --- parent-issue-hierarchy: missing-parent fetch after the streamed fetch ----------------------

namespace {
CachedTicket MakeChildTicket(const std::string& id, const std::string& parentRef) {
    CachedTicket t = MakeTicket(id, "child of " + parentRef);
    t.fieldValues["parent"] = parentRef;
    return t;
}
} // namespace

TEST_CASE("TicketSyncService fetches parents referenced by streamed rows but absent from the result") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9 - Big epic"));
    scripted.push_back(MakeChildTicket("CHILD-2", "EPIC-9"));
    scripted.push_back(MakeChildTicket("CHILD-3", "CHILD-1")); // present parent: never requested
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> parents;
    parents.push_back(MakeTicket("EPIC-9", "Big epic"));
    fake->SetFetchIssuesForKeysResult(true, parents);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 4; }));
    CHECK(fake->FetchIssuesForKeysCallCount() == 1);
    REQUIRE(fake->FetchIssuesForKeysLastKeys().size() == 1);
    CHECK(fake->FetchIssuesForKeysLastKeys()[0] == "EPIC-9");

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "EPIC-9", got));
    CHECK(got.fieldValues["summary"] == "Big epic");
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService chases a multi-level ancestor chain within one sync") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    // Only the leaf is streamed; its parent, grandparent and great-grandparent are all absent.
    scripted.push_back(MakeChildTicket("SUBTASK-1", "TASK-1"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    // One scripted set covering every hop: the fake filters it down to whatever keys each
    // hop actually requests (TASK-1, then STORY-1, then EPIC-1).
    std::vector<CachedTicket> ancestors;
    ancestors.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    ancestors.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    ancestors.push_back(MakeTicket("EPIC-1", "Root epic")); // no parent: chain ends here
    fake->SetFetchIssuesForKeysResult(true, ancestors);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 4; }));
    // Three hops: TASK-1, then STORY-1, then EPIC-1 — one round-trip per level, all in this sync.
    CHECK(fake->FetchIssuesForKeysCallCount() == 3);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "TASK-1", got));
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got));
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "EPIC-1", got));
    CHECK(got.fieldValues["summary"] == "Root epic");
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

// --- parent-issue-hierarchy: missing-CHILDREN fetch after the streamed fetch --------------------
// The downward mirror of the tests above: a view like `key = EPIC-1` streams only the epic
// itself, but the tree should still show its descendants without a separate sync per level.

TEST_CASE("TicketSyncService chases a full descendant tree within one sync") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    // Only the epic is streamed — as if the view's JQL were `key = EPIC-1`.
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    // One scripted set covering the whole descendant tree: the fake filters it down to
    // whatever parent keys each hop actually requests.
    std::vector<CachedTicket> descendants;
    descendants.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    descendants.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    descendants.push_back(MakeChildTicket("SUBTASK-1", "TASK-1")); // leaf: no children of its own
    fake->SetFetchChildrenOfKeysResult(true, descendants);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 4; }));
    // Four hops: children-of-EPIC-1, -STORY-1, -TASK-1 each find one descendant; the fourth
    // (children-of-SUBTASK-1) comes back empty. Unlike the ancestor direction — which can tell
    // "nothing new" from parent fields already in hand, with no network call — there is no
    // local signal for "does this ticket have children", so confirming a leaf always costs one
    // more (empty) round-trip.
    CHECK(fake->FetchChildrenOfKeysCallCount() == 4);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got));
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "TASK-1", got));
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "SUBTASK-1", got));
    CHECK(got.fieldValues["summary"] == "child of TASK-1");
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService surfaces a soft warning when the children fetch fails; streamed rows still land") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchChildrenOfKeysError(TrackerErrorTransport("timeout"));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK(fake->FetchChildrenOfKeysCallCount() == 1);
    CHECK(deps.LastTrackerTicketSyncWarning.find("children of") != std::string::npos);
    CHECK(deps.LastTrackerTicketSyncWarning.find("timeout") != std::string::npos);
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "EPIC-1", got));

    svc.CancelAndJoinActiveStreamingSync();
}

// Pillar 6: a descendant / ancestor walk that failed did not see every row the view holds through the
// hierarchy, so the sync must not purge rows an earlier, complete walk cached. The control case proves
// the purge does run here when the walk completes.
TEST_CASE("TicketSyncService keeps cached descendants when the children fetch fails") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeChildTicket("STORY-1", "EPIC-1")); // cached by an earlier sync
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchChildrenOfKeysError(TrackerErrorTransport("timeout"));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    SpinUntil(svc, []() { return false; }, 50); // let any stale purge run to completion
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got));
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "EPIC-1", got));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService purges a cached row the completed descendant walk no longer reaches") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeChildTicket("STORY-1", "EPIC-1"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchChildrenOfKeysResult(true, std::vector<CachedTicket>()); // the walk finds no children now

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    CachedTicket got;
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && !deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got); }));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService keeps cached ancestors when the parent fetch fails") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("EPIC-9", "cached by an earlier sync"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchIssuesForKeysError(TrackerErrorTransport("timeout"));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    SpinUntil(svc, []() { return false; }, 50);
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "EPIC-9", got));
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "CHILD-1", got));

    svc.CancelAndJoinActiveStreamingSync();
}

// A key the tracker refuses on its own is a definitive answer that repeats on every sync; leaving the
// purge off for it would never purge rows deleted on the server. The refused key is named.
TEST_CASE("TicketSyncService still purges when the tracker refuses a single key's children outright") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeChildTicket("STORY-1", "EPIC-1"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchChildrenOfKeysError(TrackerErrorInvalidRequest("bad key batch", 400));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    CachedTicket got;
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && !deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got); }));
    CHECK(deps.LastTrackerTicketSyncWarning.find("refused by the tracker: EPIC-1") != std::string::npos);

    svc.CancelAndJoinActiveStreamingSync();
}

// One key the tracker cannot browse rejects the whole batch (Jira's `key in (...)` 400). The walk
// isolates it, still loads and keeps every valid ancestor, and stays authoritative for the purge.
TEST_CASE("TicketSyncService isolates a refused parent key and keeps the valid ancestors") {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("EPIC-9", "cached by an earlier sync"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("GONE-1", "no longer in the view or its hierarchy"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9"));
    scripted.push_back(MakeChildTicket("CHILD-2", "BAD-1"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> parents;
    parents.push_back(MakeTicket("EPIC-9", "Epic"));
    fake->SetFetchIssuesForKeysResult(true, parents);
    fake->SetFetchIssuesForKeysRefusedKeys({"BAD-1"});

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    CachedTicket got;
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && !deps.CacheImpl->TryGetTicket("Jira", "GONE-1", got); }));
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "EPIC-9", got));
    CHECK(deps.LastTrackerTicketSyncWarning.find("refused by the tracker: BAD-1") != std::string::npos);
    CHECK(fake->FetchIssuesForKeysCallsFor("EPIC-9") >= 2); // the batch, then its own half

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService isolates two refused parent keys in one wide hop") {
    // 40 parents in one hop, two of them refused: a fixed budget of 16 extra requests ran out before the
    // second refused key was isolated, which kept the stale purge off for the view on every sync.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("GONE-1", "no longer in the view or its hierarchy"));
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    std::vector<CachedTicket> parents;
    for (int i = 0; i < 40; ++i) {
        const std::string parentKey = i == 5 ? "BAD-1" : i == 30 ? "BAD-2" : "P-" + std::to_string(i);
        scripted.push_back(MakeChildTicket("CHILD-" + std::to_string(i), parentKey));
        if (parentKey.compare(0, 2, "P-") == 0) {
            parents.push_back(MakeTicket(parentKey, "Parent"));
        }
    }
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchIssuesForKeysResult(true, parents);
    fake->SetFetchIssuesForKeysRefusedKeys({"BAD-1", "BAD-2"});

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    CachedTicket got;
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && !deps.CacheImpl->TryGetTicket("Jira", "GONE-1", got); }));
    CHECK(deps.ActiveTicketsImpl.size() == 40 + 38); // every child and every parent that was not refused
    CHECK(deps.LastTrackerTicketSyncWarning.find("refused by the tracker: BAD-1 BAD-2") != std::string::npos);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService does not re-query children it already asked about in this sync") {
    // The view already streams the whole tree: one hop asks about every streamed id, and the children
    // it returns were all queried in that hop, so there is no second hop.
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> tree;
    tree.push_back(MakeTicket("EPIC-1", "Root epic"));
    tree.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    tree.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    fake->SetFetchIssuesResult(tree, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> descendants;
    descendants.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    descendants.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    fake->SetFetchChildrenOfKeysResult(true, descendants);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 3; }));
    CHECK(fake->FetchChildrenOfKeysCallCount() == 1);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService fetches children even when the active view hides parents") {
    // Deliberately the OPPOSITE gating from the ancestor fetch (see
    // "TicketSyncService skips the parent fetch when the active view hides parents" below):
    // hide-parents keeps exactly the leaf descendants this fetch supplies, so skipping the
    // fetch here would defeat the toggle instead of saving work it made pointless.
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> children;
    children.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    fake->SetFetchChildrenOfKeysResult(true, children);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    ViewDefinition leafOnly;
    leafOnly.Id = "leaf-only";
    leafOnly.Name = "Leaf only";
    leafOnly.HideParents = true;
    views.Views.push_back(leafOnly);
    views.ActiveViewId = leafOnly.Id;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 2; }));
    CHECK(fake->FetchChildrenOfKeysCallCount() >= 1);
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "STORY-1", got));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService skips the children fetch when LoadParentIssues is off") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> children;
    children.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    fake->SetFetchChildrenOfKeysResult(true, children);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    cfg.LoadParentIssues = false;
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK(fake->FetchChildrenOfKeysCallCount() == 0);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService surfaces a soft warning when the parent fetch fails; streamed rows still land") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9"));
    scripted.push_back(MakeTicket("LEAF-1", "leaf"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    fake->SetFetchIssuesForKeysError(TrackerErrorTransport("timeout"));

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 2; }));
    CHECK(fake->FetchIssuesForKeysCallCount() == 1);
    CHECK(deps.LastTrackerTicketSyncWarning.find("parent issue(s) could not be loaded") != std::string::npos);
    CHECK(deps.LastTrackerTicketSyncWarning.find("timeout") != std::string::npos);
    CachedTicket got;
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "CHILD-1", got));
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "LEAF-1", got));
    CHECK_FALSE(deps.CacheImpl->TryGetTicket("Jira", "EPIC-9", got));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService skips the parent fetch when the active view hides parents") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    ViewDefinition leafOnly;
    leafOnly.Id = "leaf-only";
    leafOnly.Name = "Leaf only";
    leafOnly.HideParents = true;
    views.Views.push_back(leafOnly);
    views.ActiveViewId = leafOnly.Id;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK(fake->FetchIssuesForKeysCallCount() == 0);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService skips the parent fetch when LoadParentIssues is off") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("CHILD-1", "EPIC-9"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    cfg.LoadParentIssues = false;
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK(fake->FetchIssuesForKeysCallCount() == 0);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

// --- delta-sync Phase 1: cancellation reaches the hierarchy walks, epoch + session stats ---------

TEST_CASE("TicketSyncService parent walk passes its cancel token into the keyed fetch and stops when it fires") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeChildTicket("SUBTASK-1", "TASK-1"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> ancestors;
    ancestors.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    ancestors.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    ancestors.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesForKeysResult(true, ancestors);

    TicketSyncService svc(deps);
    // The user supersedes the sync while the first hop's request is "on the wire".
    fake->SetOnKeyedFetch([&svc]() { svc.RequestCancel(); });

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && fake->FetchIssuesForKeysCancellableCallCount() >= 1; }));
    // The walk used the cancellable entry point, the token it handed over reported the cancel, and
    // the later hops (STORY-1, EPIC-1) were never requested.
    CHECK(fake->FetchIssuesForKeysCancellableCallCount() == 1);
    CHECK(fake->KeyedFetchTokenReportedCancelled());
    CHECK(fake->FetchIssuesForKeysCallCount() == 1);
    // A cancelled walk is not a failed one: nothing is reported to the user.
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService child walk passes its cancel token into the keyed fetch and stops when it fires") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("EPIC-1", "Root epic"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    std::vector<CachedTicket> descendants;
    descendants.push_back(MakeChildTicket("STORY-1", "EPIC-1"));
    descendants.push_back(MakeChildTicket("TASK-1", "STORY-1"));
    fake->SetFetchChildrenOfKeysResult(true, descendants);

    TicketSyncService svc(deps);
    fake->SetOnKeyedFetch([&svc]() { svc.RequestCancel(); });

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && fake->FetchChildrenOfKeysCancellableCallCount() >= 1; }));
    CHECK(fake->FetchChildrenOfKeysCancellableCallCount() == 1);
    CHECK(fake->KeyedFetchTokenReportedCancelled());
    CHECK(fake->FetchChildrenOfKeysCallCount() == 1);
    CHECK(deps.LastTrackerTicketSyncWarning.empty());

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService::SessionEpoch moves once per started streaming session") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult({MakeTicket("ABC-1")}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    CHECK(svc.SessionEpoch() == 0);

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    CHECK(svc.SessionEpoch() == 1);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK(svc.SessionEpoch() == 1); // finishing a session does not move it

    svc.SyncWithBackend(&cfg, &views);
    CHECK(svc.SessionEpoch() == 2);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService records what a finished session cost") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult({MakeTicket("ABC-1"), MakeTicket("ABC-2")}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    CHECK_FALSE(svc.LastSessionStats().Valid); // nothing has finished yet

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.LastSessionStats().Valid; }));

    const smatchet::sync::SyncSessionStats stats = svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.BodiesFetched == 2);
    CHECK(stats.Completed);
    CHECK(stats.Error.empty());
    // A default (Delta) request against a backend that cannot list versions (the fake's default) runs the
    // full fetch and says why.
    CHECK(stats.FallbackReason.find("cannot list ticket versions") != std::string::npos);
    CHECK(stats.Requests == 5); // the fake samples +5 requests per call: start vs end
    CHECK(stats.DownloadBytes == 500);
    CHECK(stats.RequestId != 0);

    svc.CancelAndJoinActiveStreamingSync();
}

// --- delta-sync Phase 2: every path that saves a synced row also saves the tracker version ---

TEST_CASE("TicketSyncService stores the tracker version beside each streamed and applied row") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    CachedTicket streamed = MakeTicket("ABC-1");
    streamed.fieldValues["updated"] = "2024-05-01T10:00:00.000+0000";
    CachedTicket unstamped = MakeTicket("ABC-2"); // no `updated`: stored without meta
    fake->SetFetchIssuesResult({streamed, unstamped}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 2; }));

    auto meta = deps.CacheImpl->GetTicketSyncMeta("Jira");
    REQUIRE(meta.count("ABC-1") == 1u);
    CHECK(meta.at("ABC-1").RemoteUpdated == "2024-05-01T10:00:00.000+0000");
    CHECK(meta.at("ABC-1").SyncedAtEpochSec > 0);
    CHECK(meta.at("ABC-1").Fields == std::vector<std::string>{"status", "summary", "updated"});
    CHECK(meta.count("ABC-2") == 0u);

    // ApplyIssueFetchPack (the inline-fetch path) stamps too.
    TrackerIssueFetchPack pack;
    CachedTicket applied = MakeTicket("ABC-3");
    applied.fieldValues["updated"] = "2024-06-01T10:00:00.000+0000";
    pack.Tickets.push_back(applied);
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);
    meta = deps.CacheImpl->GetTicketSyncMeta("Jira");
    CHECK(meta.count("ABC-3") == 1u);

    svc.CancelAndJoinActiveStreamingSync();
}

// --- delta-sync Phase 3: roster-driven reload with a full-fetch fallback --------------------------

namespace {

CachedTicket Stamped(const std::string& id, const std::string& updated, const std::string& summary = "cached") {
    CachedTicket t = MakeTicket(id, summary);
    t.fieldValues["updated"] = updated;
    return t;
}

TrackerRoster MakeRoster(const std::vector<std::pair<std::string, std::string>>& entries, bool complete = true) {
    TrackerRoster roster;
    for (const auto& kv : entries) {
        TrackerRosterEntry e;
        e.Key = kv.first;
        e.Updated = kv.second;
        roster.Entries.push_back(e);
    }
    roster.Complete = complete;
    if (!complete) {
        roster.Warning = "page cap reached";
    }
    return roster;
}

// A service wired to a fake backend that supports rosters, with a Jira-keyed cache the tests can seed.
struct DeltaRig {
    FakeTicketSyncDeps deps;
    FakeTrackerClient* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    TicketSyncService svc{deps};
    TrackerConfig cfg;
    ViewsStore views;

    DeltaRig() {
        cfg.TrackerType = "fake";
        fake->SetRosterSupported(true);
    }
    ~DeltaRig() { svc.CancelAndJoinActiveStreamingSync(); }

    // Cached rows as an earlier sync would have left them: row + the version it was fetched at.
    void Seed(const std::vector<CachedTicket>& rows) {
        std::vector<TicketSyncMeta> metas;
        for (const CachedTicket& row : rows) {
            metas.push_back(smatchet::sync::BuildSyncMeta(row, static_cast<std::int64_t>(std::time(nullptr))));
        }
        deps.CacheImpl->SaveTicketsWithSyncMeta("Jira", rows, metas);
    }
    void Sync(smatchet::sync::SyncMode mode = smatchet::sync::SyncMode::Delta) {
        svc.SyncWithBackend(&cfg, &views, mode);
    }
    // Waits for the started session to finish (worker, apply queue and stale deletion).
    bool Settle() {
        return SpinUntil(svc,
                         [&]() { return !svc.IsActive() && svc.LastSessionStats().RequestId == svc.SessionEpoch(); });
    }
    std::size_t CachedCount() { return deps.CacheImpl->GetAllTicketIds("Jira").size(); }
};

} // namespace

TEST_CASE("Delta reload: an unchanged view downloads no ticket bodies") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1"), Stamped("ABC-3", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T1"}, {"ABC-3", "T1"}}));

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterCallCount() == 1);
    CHECK(rig.fake->FetchIssuesCallCount() == 0);        // no streamed full fetch
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0); // and no keyed body download
    CHECK(rig.deps.ActiveTicketsImpl.size() == 3);       // the grid is filled from the cache
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "delta");
    CHECK(stats.RosterSize == 3);
    CHECK(stats.Unchanged == 3);
    CHECK(stats.BodiesFetched == 0);
    CHECK(stats.FallbackReason.empty());
    CHECK(stats.Completed);
}

TEST_CASE("Delta reload: only changed and new tickets are downloaded") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}, {"ABC-4", "T1"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh"), Stamped("ABC-4", "T1", "new")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 0);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 1);
    std::vector<std::string> asked = rig.fake->FetchIssuesForKeysLastKeys();
    std::sort(asked.begin(), asked.end());
    CHECK(asked == std::vector<std::string>{"ABC-2", "ABC-4"});
    CHECK(rig.deps.ActiveTicketsImpl.size() == 3);
    CachedTicket got;
    REQUIRE(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldValues["summary"] == "fresh");
    CHECK(rig.deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-2").RemoteUpdated == "T2");
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "delta");
    CHECK(stats.BodiesFetched == 2);
    CHECK(stats.Unchanged == 1);
}

TEST_CASE("Delta reload: a roster error falls back to exactly one full fetch") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRosterError(TrackerErrorTransport("roster down"));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterCallCount() == 1);
    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0);
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.find("roster") != std::string::npos);
    CHECK(rig.deps.LastTrackerTicketSyncWarning.empty()); // the fallback is invisible to the user
    REQUIRE(rig.deps.ActiveTicketsImpl.size() == 1);
    CHECK(rig.deps.ActiveTicketsImpl[0].fieldValues["summary"] == "from full");
}

TEST_CASE("Delta reload: a body download error falls back to a full fetch") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysError(TrackerErrorTransport("boom"));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T2", "from full")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.find("download") != std::string::npos);
    CachedTicket got;
    REQUIRE(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldValues["summary"] == "from full");
}

TEST_CASE("Delta reload: a forced Full skips the roster entirely") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterCallCount() == 0);
    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.empty()); // requested, not a fallback
}

TEST_CASE("Delta reload: an incomplete roster does not purge rows it did not list") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-9", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}, /*complete=*/false));

    rig.Sync();
    REQUIRE(rig.Settle());

    CachedTicket got;
    CHECK(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-9", got)); // not proven gone
    CHECK_FALSE(rig.svc.LastSessionStats().Completed);
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
}

TEST_CASE("Delta reload: a complete roster purges cached rows that left the view") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-9", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}, /*complete=*/true));

    rig.Sync();
    REQUIRE(rig.Settle());

    CachedTicket got;
    CHECK_FALSE(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-9", got));
    CHECK(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(rig.deps.CacheImpl->GetTicketSyncMeta("Jira").count("ABC-9") == 0u);
}

TEST_CASE("Delta reload: cancelling during the roster request stops without a fallback fetch") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetOnRoster([&rig]() { rig.svc.RequestCancel(); });

    rig.Sync();
    REQUIRE(SpinUntil(rig.svc, [&]() { return !rig.svc.IsActive() && rig.fake->FetchRosterCallCount() >= 1; }));

    CHECK(rig.fake->FetchIssuesCallCount() == 0);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0);
    CHECK(rig.deps.LastTrackerTicketSyncWarning.empty());
}

TEST_CASE("Delta reload: a backend that cannot list versions keeps the old full fetch") {
    DeltaRig rig;
    rig.fake->SetRosterSupported(false);
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterCallCount() == 0);
    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK_FALSE(rig.svc.LastSessionStats().FallbackReason.empty());
}

TEST_CASE("Delta reload: an unreadable version table is reported as its own fallback, not as 'no cached versions'") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.deps.CacheImpl->FailGetSyncMeta(true);
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterCallCount() == 0); // the delta attempt was not made
    CHECK(rig.fake->FetchIssuesCallCount() == 1); // the full fetch ran and the sync did not fail
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.find("could not read cached ticket versions") != std::string::npos);
    CHECK(stats.FallbackReason.find("scripted meta read failure") != std::string::npos);
    CHECK(stats.FallbackReason.find("no cached ticket versions yet") == std::string::npos);
}

TEST_CASE("Delta reload: the config kill switch and an empty version table both mean a full fetch") {
    {
        DeltaRig rig;
        rig.cfg.DeltaSyncEnabled = false;
        rig.Seed({Stamped("ABC-1", "T1")});
        rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
        rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);
        rig.Sync();
        REQUIRE(rig.Settle());
        CHECK(rig.fake->FetchRosterCallCount() == 0);
        CHECK(rig.fake->FetchIssuesCallCount() == 1);
    }
    {
        DeltaRig rig; // rows cached by an older build: no versions known
        rig.deps.CacheImpl->SaveTicket("Jira", Stamped("ABC-1", "T1"));
        rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
        rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);
        rig.Sync();
        REQUIRE(rig.Settle());
        CHECK(rig.fake->FetchRosterCallCount() == 0);
        CHECK(rig.fake->FetchIssuesCallCount() == 1);
    }
}

TEST_CASE("Delta reload: a view that mostly changed switches to one full fetch") {
    DeltaRig rig;
    std::vector<CachedTicket> rows;
    std::vector<std::pair<std::string, std::string>> roster;
    std::vector<CachedTicket> fresh;
    for (int i = 0; i < 150; ++i) {
        const std::string id = "ABC-" + std::to_string(i + 1);
        rows.push_back(Stamped(id, "T1"));
        roster.push_back({id, "T2"}); // every ticket moved
        fresh.push_back(Stamped(id, "T2", "fresh"));
    }
    rig.Seed(rows);
    rig.fake->SetRoster(MakeRoster(roster));
    rig.fake->SetFetchIssuesResult(fresh, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0);
    CHECK(rig.svc.LastSessionStats().FallbackReason.find("most of the view") != std::string::npos);
}

TEST_CASE("Delta reload: an empty roster over a non-empty cache keeps the cache (empty-sync guard)") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({}, /*complete=*/true));

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.CachedCount() == 2); // a zero-row "complete" view is suspect and never wipes on its own
}

TEST_CASE("Delta reload: a roster key that comes back with no row is kept but makes the walk non-authoritative") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-9", "T1")});
    // ABC-5 is listed but the keyed download returns nothing for it (deleted between the two requests).
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-5", "T1"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK_FALSE(rig.svc.LastSessionStats().Completed); // not authoritative: no purge
    CachedTicket got;
    CHECK(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-9", got));
}

// --- delta-sync Phase 4: SQLite stays off the UI thread ----------------------------------------------

TEST_CASE("A fetched batch is persisted by the worker before the UI thread applies it") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "one"), Stamped("ABC-2", "T1", "two")}, true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    // Wait for the rows to reach the cache WITHOUT ticking the UI-thread apply.
    bool persisted = false;
    for (int i = 0; i < 500 && !persisted; ++i) {
        persisted = deps.CacheImpl->GetAllTicketIds("Jira").size() == 2;
        if (!persisted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    REQUIRE(persisted);
    CHECK(deps.ActiveTicketsImpl.empty()); // nothing applied yet: the drain has not run
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").count("ABC-1") == 1u);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 2; }));
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("A fetched row older than the cached version never replaces the cached row") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    const CachedTicket newer = Stamped("ABC-1", "2024-05-01T10:00:00.000+0000", "newer");
    deps.CacheImpl->SaveTicketsWithSyncMeta(
        "Jira", {newer}, {smatchet::sync::BuildSyncMeta(newer, static_cast<std::int64_t>(std::time(nullptr)))});
    fake->SetFetchIssuesResult({Stamped("ABC-1", "2024-05-01T09:00:00.000+0000", "older"),
                                Stamped("ABC-2", "2024-05-01T09:00:00.000+0000", "two")},
                               true);

    deps.ActiveTicketsImpl.push_back(newer); // the newer copy is already on screen
    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.LastSessionStats().Valid; }));

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "newer");
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-1").RemoteUpdated == "2024-05-01T10:00:00.000+0000");
    // The dropped row is still part of the view: the resident newer copy survives the roster convergence.
    bool residentKept = false;
    for (const CachedTicket& t : deps.ActiveTicketsImpl) {
        residentKept = residentKept || (t.id == "ABC-1" && t.fieldValues.at("summary") == "newer");
    }
    CHECK(residentKept);
    CHECK(deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got)); // the other row still lands
    svc.CancelAndJoinActiveStreamingSync();
}

namespace {

// Caches ABC-1 at `storedStamp` ("stored"), then runs one `mode` sync whose fetch returns ABC-1 stamped
// 2024-05-01T09:00Z ("fetched"). The caller inspects the cache afterwards.
void SyncFetchedRowOverStored(FakeTicketSyncDeps& deps, const std::string& storedStamp, smatchet::sync::SyncMode mode) {
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    const CachedTicket stored = Stamped("ABC-1", storedStamp, "stored");
    deps.CacheImpl->SaveTicketsWithSyncMeta(
        "Jira", {stored}, {smatchet::sync::BuildSyncMeta(stored, static_cast<std::int64_t>(std::time(nullptr)))});
    fake->SetFetchIssuesResult({Stamped("ABC-1", "2024-05-01T09:00:00.000+0000", "fetched")}, true);
    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views, mode);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.LastSessionStats().Valid; }));
    svc.CancelAndJoinActiveStreamingSync();
}

} // namespace

// Version-guard wedge repair (docs/plans/active/delta-sync.md § Review follow-ups): a stored stamp ahead of the
// tracker's real ones must not drop every refetch of the row forever.
TEST_CASE("A Full sync overwrites a row whose stored stamp is later than the fetched one") {
    FakeTicketSyncDeps deps;
    SyncFetchedRowOverStored(deps, "2024-05-02T10:00:00.000+0000", smatchet::sync::SyncMode::Full);
    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "fetched");
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-1").RemoteUpdated == "2024-05-01T09:00:00.000+0000");
}

TEST_CASE("A Delta sync still refuses a fetched row older than the stored stamp") {
    FakeTicketSyncDeps deps;
    SyncFetchedRowOverStored(deps, "2024-05-02T10:00:00.000+0000", smatchet::sync::SyncMode::Delta);
    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "stored");
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-1").RemoteUpdated == "2024-05-02T10:00:00.000+0000");
}

TEST_CASE("A stored stamp far in the future does not block a Delta sync write") {
    FakeTicketSyncDeps deps;
    SyncFetchedRowOverStored(deps, "2999-01-01T00:00:00.000+0000", smatchet::sync::SyncMode::Delta);
    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "fetched");
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-1").RemoteUpdated == "2024-05-01T09:00:00.000+0000");
}

// ApplyIssueFetchPack must not borrow the authority of an earlier Full streaming session on the same service.
TEST_CASE("After a Full session an inline fetch pack older than the stored stamp does not overwrite") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult({Stamped("ABC-9", "2024-05-01T09:00:00.000+0000", "full")}, true);
    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views, smatchet::sync::SyncMode::Full);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.LastSessionStats().Valid; }));

    const CachedTicket newer = Stamped("ABC-1", "2024-05-02T10:00:00.000+0000", "newer");
    deps.CacheImpl->SaveTicketsWithSyncMeta(
        "Jira", {newer}, {smatchet::sync::BuildSyncMeta(newer, static_cast<std::int64_t>(std::time(nullptr)))});
    TrackerIssueFetchPack pack;
    pack.Tickets.push_back(Stamped("ABC-1", "2024-05-01T09:00:00.000+0000", "older"));
    pack.FullSyncCompleted = false;
    svc.ApplyIssueFetchPack(pack);

    CachedTicket got;
    REQUIRE(deps.CacheImpl->TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "newer");
    CHECK(deps.CacheImpl->GetTicketSyncMeta("Jira").at("ABC-1").RemoteUpdated == "2024-05-02T10:00:00.000+0000");
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("The stale sweep is one worker-side batch delete plus one in-memory pass") {
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    for (int i = 1; i <= 25; ++i) {
        deps.CacheImpl->SaveTicket("Jira", MakeTicket("OLD-" + std::to_string(i)));
    }
    fake->SetFetchIssuesResult({MakeTicket("NEW-1")}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.CacheImpl->GetAllTicketIds("Jira").size() == 1; }));

    CHECK(deps.CacheImpl->DeleteTicketsCallCount() == 1); // 25 rows, ONE transaction
    CHECK(deps.BackgroundTasksLaunched == 1);             // and it ran as a background task
    REQUIRE(deps.ActiveTicketsImpl.size() == 1);
    CHECK(deps.ActiveTicketsImpl[0].id == "NEW-1");
    svc.CancelAndJoinActiveStreamingSync();
}

// --- delta-sync Phase 5: the parent and child walks list versions first ---------------------------------

namespace {
CachedTicket StampedChild(const std::string& id, const std::string& parentRef, const std::string& updated,
                          const std::string& summary = "cached") {
    CachedTicket t = Stamped(id, updated, summary);
    t.fieldValues["parent"] = parentRef;
    return t;
}
} // namespace

TEST_CASE("Delta reload: an unchanged ancestor is served from the cache, not downloaded") {
    DeltaRig rig;
    rig.Seed({StampedChild("SUB-1", "EPIC-1", "T1"), Stamped("EPIC-1", "T1", "epic")});
    rig.fake->SetRoster(MakeRoster({{"SUB-1", "T1"}})); // the view itself holds only the sub-task
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T1", "epic")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterForKeysCallCount() == 1);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0); // no ancestor body download
    CHECK(rig.deps.ActiveTicketsImpl.size() == 2);       // but the epic is on the grid (from the cache)
    CHECK(rig.svc.LastSessionStats().BodiesFetched == 0);
}

TEST_CASE("Delta reload: only a changed ancestor is downloaded") {
    DeltaRig rig;
    rig.Seed({StampedChild("SUB-1", "EPIC-1", "T1"), Stamped("EPIC-1", "T1", "old epic")});
    rig.fake->SetRoster(MakeRoster({{"SUB-1", "T1"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T2", "new epic")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 1);
    REQUIRE(rig.fake->FetchIssuesForKeysLastKeys().size() == 1);
    CHECK(rig.fake->FetchIssuesForKeysLastKeys()[0] == "EPIC-1");
    CachedTicket got;
    REQUIRE(rig.deps.CacheImpl->TryGetTicket("Jira", "EPIC-1", got));
    CHECK(got.fieldValues["summary"] == "new epic");
}

TEST_CASE("Delta reload: a hierarchy roster error runs today's full-body hop") {
    DeltaRig rig;
    rig.Seed({StampedChild("SUB-1", "EPIC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"SUB-1", "T1"}}));
    rig.fake->SetHierarchyRosterError(TrackerErrorTransport("roster hiccup"));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T1", "epic")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 1); // the full-body hop
    CHECK(rig.deps.ActiveTicketsImpl.size() == 2);
    CHECK(rig.deps.LastTrackerTicketSyncWarning.empty());
}

TEST_CASE("Delta reload: an unchanged descendant tree is served from the cache") {
    DeltaRig rig;
    rig.Seed({Stamped("EPIC-1", "T1", "epic"), StampedChild("STORY-1", "EPIC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"EPIC-1", "T1"}}));
    rig.fake->SetFetchChildrenOfKeysResult(true, {StampedChild("STORY-1", "EPIC-1", "T1")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchChildRosterCallCount() >= 1);
    CHECK(rig.fake->FetchChildrenOfKeysCallCount() == 0); // no child body download
    CHECK(rig.deps.ActiveTicketsImpl.size() == 2);
}

TEST_CASE("A forced Full sync keeps the hierarchy walks on full bodies") {
    DeltaRig rig;
    rig.Seed({StampedChild("SUB-1", "EPIC-1", "T1"), Stamped("EPIC-1", "T1", "epic")});
    rig.fake->SetFetchIssuesResult({StampedChild("SUB-1", "EPIC-1", "T1")}, true);
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T1", "epic")});

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchRosterForKeysCallCount() == 0);
    CHECK(rig.fake->FetchChildRosterCallCount() == 0);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 1);
}

// --- delta-sync review fixes ---------------------------------------------------------------------------

TEST_CASE("Delta reload: every body refused while the roster is fine never purges; it falls back to a full fetch") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T2"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-1", "ABC-2"});
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T2", "from full"), Stamped("ABC-2", "T2", "from full")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 1);
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.find("refused") != std::string::npos);
    CHECK(rig.CachedCount() == 2); // nothing was purged on the strength of the refusals
    CHECK(rig.deps.ActiveTicketsImpl.size() == 2);
}

TEST_CASE("Delta reload: one refused body is not treated as gone; the full fetch decides") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1"), Stamped("ABC-3", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}, {"ABC-3", "T2"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-3", "T2", "fresh")});
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-2"});
    rig.fake->SetFetchIssuesResult(
        {Stamped("ABC-1", "T1"), Stamped("ABC-2", "T2", "from full"), Stamped("ABC-3", "T2", "fresh")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK(rig.svc.LastSessionStats().FallbackReason.find("ABC-2") != std::string::npos);
    CachedTicket got;
    REQUIRE(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldValues["summary"] == "from full");
    CHECK(rig.CachedCount() == 3);
}

TEST_CASE("Delta reload: a refused roster-listed ancestor keeps the cache and makes the walk non-authoritative") {
    DeltaRig rig;
    rig.Seed({StampedChild("SUB-1", "EPIC-1", "T1"), Stamped("EPIC-1", "T1", "old epic"), Stamped("OTHER-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"SUB-1", "T1"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T2", "new epic")});
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"EPIC-1"});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 0); // the rows were queued already: no second full queue
    CachedTicket got;
    CHECK(rig.deps.CacheImpl->TryGetTicket("Jira", "EPIC-1", got));
    CHECK(rig.deps.CacheImpl->TryGetTicket("Jira", "OTHER-1", got)); // no purge on a non-authoritative walk
    CHECK(rig.deps.LastTrackerTicketSyncWarning.find("EPIC-1") != std::string::npos);
}

TEST_CASE("Delta reload: a failing save of the changed rows neither falls back nor purges") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh")});
    rig.deps.CacheImpl->FailSaveWithMetaOnCall(1); // the changed batch's save

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesCallCount() == 0); // no second full queue on top of the unchanged rows
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.CachedCount() == 2);
    CHECK(rig.deps.ActiveTicketsImpl.size() >= 1);
}

TEST_CASE("A queued Full survives a later Delta request") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, true);

    rig.Sync(smatchet::sync::SyncMode::Delta);
    REQUIRE(rig.svc.IsActive());
    rig.Sync(smatchet::sync::SyncMode::Full);  // queued behind the running session
    rig.Sync(smatchet::sync::SyncMode::Delta); // must not downgrade it
    REQUIRE(rig.Settle());

    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.empty()); // requested, not a fallback
    CHECK(rig.fake->FetchIssuesCallCount() >= 1);
}

namespace {
/// A rig whose first Full session is parked inside its ancestor fetch (the view's rows are on the wire) until
/// `release` is set, so a test can issue a second request "mid-Full" without relying on timing. The token the parked
/// fetch sampled once it was released is kept in `firstFetchCancelled`: true means the request superseded the Full.
struct ParkedFullRig : DeltaRig {
    std::atomic<bool> inFetch{false};
    std::atomic<bool> release{false};
    std::atomic<bool> firstFetchCancelled{false};
    std::atomic<int> tokenSamples{0};

    ~ParkedFullRig() { release.store(true); } // a failed REQUIRE must not leave the worker parked

    /// Scripts `fake` (whichever backend is current), starts a Full and waits until its fetch is on the wire.
    /// Returns the epoch of that Full session.
    std::uint64_t StartParkedFull() {
        fake->SetRoster(MakeRoster({{"SUB-1", "T1"}}));
        fake->SetFetchIssuesResult({StampedChild("SUB-1", "EPIC-1", "T1")}, true);
        fake->SetFetchIssuesForKeysResult(true, {Stamped("EPIC-1", "T1", "epic")});
        fake->SetOnKeyedFetch([this]() {
            inFetch.store(true);
            for (int i = 0; i < 2000 && !release.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        fake->SetOnKeyedTokenSampled([this](bool cancelled) {
            if (tokenSamples.fetch_add(1) == 0) {
                firstFetchCancelled.store(cancelled);
            }
        });
        Sync(smatchet::sync::SyncMode::Full);
        for (int i = 0; i < 2000 && !inFetch.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return svc.SessionEpoch();
    }

    /// Lets the parked fetch go on and waits until the replacement session (a later epoch) has finished.
    bool SettleReplacement(std::uint64_t parkedEpoch) {
        release.store(true);
        return SpinUntil(
            svc,
            [&]() {
                return !svc.IsActive() && svc.SessionEpoch() > parkedEpoch &&
                       svc.LastSessionStats().RequestId == svc.SessionEpoch();
            },
            /*maxTicks=*/2000);
    }
};

/// Hands out a fresh scripted client per Create and remembers the site each was built for.
class RecordingBackendFactory : public ITrackerBackendFactory {
  public:
    std::vector<std::string> Domains;
    std::unique_ptr<ITrackerBackend> Create(const std::string& kind, const TrackerConfig& cfg) override {
        Domains.push_back(cfg.Domain);
        std::unique_ptr<FakeTrackerClient> client(new FakeTrackerClient(kind));
        client->SetFetchIssuesResult({Stamped("NEW-1", "T1")}, true);
        return std::move(client);
    }
};
} // namespace

TEST_CASE("A running Full is not downgraded by a Delta request: the replacement session runs as Full") {
    ParkedFullRig rig;
    const std::uint64_t parkedEpoch = rig.StartParkedFull();
    REQUIRE(rig.inFetch.load()); // the Full fetch is on the wire
    rig.Sync(smatchet::sync::SyncMode::Delta);
    REQUIRE(rig.SettleReplacement(parkedEpoch));

    CHECK(rig.firstFetchCancelled.load()); // the running Full was superseded, not left to finish
    CHECK(rig.svc.SessionEpoch() == parkedEpoch + 1);
    CHECK(rig.svc.LastSessionStats().Mode == "full"); // the queued Delta combined with the Full: sticky
    CHECK(rig.svc.LastSessionStats().RequestId == parkedEpoch + 1);
    CHECK(rig.fake->FetchRosterCallCount() == 0); // no Delta session ran
    CHECK(rig.fake->FetchIssuesCallCount() == 2); // the Full, then its replacement
}

TEST_CASE("A site switch requested mid-Full supersedes it: the new site's own session starts promptly") {
    ParkedFullRig rig;
    RecordingBackendFactory* factory = new RecordingBackendFactory();
    rig.deps.Factory.reset(factory);
    rig.deps.SetBackend(std::unique_ptr<ITrackerBackend>(new FakeTrackerClient("Jira")));
    rig.fake = static_cast<FakeTrackerClient*>(rig.deps.BackendImpl.get());
    rig.cfg.TrackerType = "Jira";
    rig.cfg.Domain = "a.example.com";
    const std::uint64_t parkedEpoch = rig.StartParkedFull();
    REQUIRE(rig.inFetch.load());
    REQUIRE(factory->Domains.empty()); // site A runs on the client the deps already hold

    TrackerConfig siteB = rig.cfg;
    siteB.Domain = "b.example.com";
    rig.svc.SyncWithBackend(&siteB, &rig.views, smatchet::sync::SyncMode::Delta); // "Save & Sync" to another site
    REQUIRE(rig.SettleReplacement(parkedEpoch));

    CHECK(rig.firstFetchCancelled.load()); // site A's Full did not keep streaming
    CHECK(rig.svc.SessionEpoch() == parkedEpoch + 1);
    REQUIRE(factory->Domains.size() == 1);
    CHECK(factory->Domains[0] == "b.example.com"); // the client was rebuilt for the new site
    REQUIRE(rig.deps.ActiveTicketsImpl.size() == 1);
    CHECK(rig.deps.ActiveTicketsImpl[0].id == "NEW-1"); // and the grid shows site B's rows, not A's
}

TEST_CASE("A view switch requested mid-Full supersedes it: the new view's own session starts promptly") {
    ParkedFullRig rig;
    rig.views.ActiveViewId = "view-a";
    const std::uint64_t parkedEpoch = rig.StartParkedFull();
    REQUIRE(rig.inFetch.load());
    CHECK(rig.fake->FetchIssuesLastActiveViewId() == "view-a");

    ViewsStore viewB = rig.views;
    viewB.ActiveViewId = "view-b";
    rig.svc.SyncWithBackend(&rig.cfg, &viewB, smatchet::sync::SyncMode::Delta); // view.activate / header view switch
    REQUIRE(rig.SettleReplacement(parkedEpoch));

    CHECK(rig.firstFetchCancelled.load()); // view A's Full did not keep streaming
    CHECK(rig.svc.SessionEpoch() == parkedEpoch + 1);
    CHECK(rig.fake->FetchIssuesLastActiveViewId() == "view-b"); // the replacement fetched the new view
}

TEST_CASE("A superseding request waits for the stale-row delete instead of starting over it") {
    DeltaRig rig;
    rig.deps.DeferBackgroundTasks = true;
    rig.Seed({Stamped("OLD-1", "T1"), Stamped("OLD-2", "T1")});
    rig.fake->SetFetchIssuesResult({Stamped("NEW-1", "T1")}, true);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(SpinUntil(rig.svc, [&]() { return !rig.deps.DeferredTasks.empty(); })); // delete parked "in flight"
    const std::uint64_t epoch = rig.svc.SessionEpoch();
    rig.Sync(smatchet::sync::SyncMode::Full); // supersedes; the delete is still running
    for (int i = 0; i < 20; ++i) {
        rig.svc.TickStreamingApply();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(rig.svc.SessionEpoch() == epoch); // not started over the pending delete

    rig.deps.RunDeferredTasks();
    REQUIRE(SpinUntil(rig.svc, [&]() { return rig.svc.SessionEpoch() > epoch && !rig.svc.IsActive(); }));
}

TEST_CASE("A stale-row delete that throws a non-std exception still releases the busy flag") {
    DeltaRig rig;
    rig.deps.DeferBackgroundTasks = true;
    rig.Seed({Stamped("OLD-1", "T1")});
    rig.fake->SetFetchIssuesResult({Stamped("NEW-1", "T1")}, true);
    rig.deps.CacheImpl->ThrowNonStdOnDelete(true);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(SpinUntil(rig.svc, [&]() { return !rig.deps.DeferredTasks.empty(); }));
    CHECK(rig.svc.IsActive());
    rig.deps.RunDeferredTasks();
    REQUIRE(SpinUntil(rig.svc, [&]() { return !rig.svc.IsActive(); }));
}

// --- delta-sync Phase 6: the change monitor's roster path ---------------------------------------------------

namespace {
struct ProbeCountingClient : FakeTrackerClient {
    int Probes = 0;
    bool Exists = true;
    Result<bool, TrackerError> ProbeIssueExists(const TrackerConfig&, const std::string&) override {
        ++Probes;
        return Result<bool, TrackerError>::Ok(Exists);
    }
};
} // namespace

TEST_CASE("Change monitor roster probe: one roster call, only changed and new bodies, vanished keys probed") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}, {"A-2", "T2"}, {"A-3", "T1"}}));
    fake.SetFetchIssuesForKeysResult(true, {Stamped("A-2", "T2", "fresh"), Stamped("A-3", "T1", "new")});
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}, {"A-2", "T1"}, {"A-9", "T1"}};
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;

    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, resident, "pane", fetched, verdicts) ==
            smatchet::sync::RosterProbeOutcome::Applied);

    CHECK(fake.FetchRosterCallCount() == 1);
    CHECK(fake.FetchIssuesForKeysCallCount() == 1);
    std::vector<std::string> asked = fake.FetchIssuesForKeysLastKeys();
    std::sort(asked.begin(), asked.end());
    CHECK(asked == std::vector<std::string>{"A-2", "A-3"}); // A-1 is unchanged: no body
    CHECK(fetched.size() == 2);
    REQUIRE(verdicts.size() == 1);
    CHECK(verdicts[0].issueKey == "A-9");
    CHECK(verdicts[0].stillExists);
    CHECK(fake.Probes == 1);
}

TEST_CASE("Change monitor roster probe: nothing changed means no body download") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}}));
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}}, "pane", fetched, verdicts) ==
            smatchet::sync::RosterProbeOutcome::Applied);
    CHECK(fake.FetchIssuesForKeysCallCount() == 0);
    CHECK(fetched.empty());
    CHECK(verdicts.empty());
}

TEST_CASE("Change monitor roster probe: a roster error skips the cycle") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    fake.SetRosterError(TrackerErrorTransport("down"));
    CHECK(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}}, "pane", fetched, verdicts) ==
          smatchet::sync::RosterProbeOutcome::Skipped);
}

TEST_CASE("Change monitor roster probe: an incomplete roster asks for the legacy path and fetches/probes nothing") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-2", "T2"}}, /*complete=*/false));
    fake.SetFetchIssuesForKeysResult(true, {Stamped("A-2", "T2")});
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    CHECK(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}}, "pane", fetched, verdicts) ==
          smatchet::sync::RosterProbeOutcome::UseLegacy);
    CHECK(fetched.empty());
    CHECK(verdicts.empty()); // a capped roster cannot prove A-1 left the view
    CHECK(fake.Probes == 0);
    CHECK(fake.FetchIssuesForKeysCallCount() == 0);
}

namespace {
/// A reader whose legacy surface (changed-since + keys-only membership) is scripted and counted, so a test can
/// see which path a monitor cycle took.
struct LegacyPathClient : ProbeCountingClient {
    int ChangedSinceCalls = 0;
    int KeysCalls = 0;
    std::vector<CachedTicket> Changed;
    std::vector<std::string> ViewKeys;
    bool ChangedSinceFails = false;
    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesChangedSince(const TrackerConfig&, const ViewsStore&,
                                                                            std::chrono::seconds,
                                                                            const std::vector<std::string>&) override {
        ++ChangedSinceCalls;
        if (ChangedSinceFails) {
            return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorTransport("down"));
        }
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(Changed);
    }
    Result<std::vector<std::string>, TrackerError> FetchIssueKeysForView(const TrackerConfig&,
                                                                         const ViewsStore&) override {
        ++KeysCalls;
        return Result<std::vector<std::string>, TrackerError>::Ok(ViewKeys);
    }
};

struct CycleResult {
    bool ok = false;
    bool addNewRows = false;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
};

CycleResult RunCycle(LegacyPathClient& fake, bool useRoster, const std::vector<std::string>& cachedKeys) {
    TrackerConfig cfg;
    ViewsStore views;
    std::unordered_map<std::string, std::string> resident;
    for (const std::string& k : cachedKeys) {
        resident[k] = "T1";
    }
    CycleResult r;
    r.ok = smatchet::sync::RunChangeProbeCycle(fake, cfg, views, useRoster, resident, cachedKeys,
                                               std::chrono::seconds(120), {"summary"}, "pane", r.fetched, r.verdicts,
                                               r.addNewRows);
    return r;
}
} // namespace

TEST_CASE("Change monitor cycle: an incomplete roster falls back to changed-since plus the keys-only reconcile") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}}, /*complete=*/false)); // the view outgrew the reload's caps
    fake.Changed = {Stamped("A-1", "T2")};
    fake.ViewKeys = {"A-1"}; // A-9 left the view or was deleted
    fake.Exists = false;

    const CycleResult r = RunCycle(fake, /*useRoster=*/true, {"A-1", "A-9"});

    REQUIRE(r.ok);
    CHECK(fake.FetchRosterCallCount() == 1);
    CHECK(fake.ChangedSinceCalls == 1);
    CHECK(fake.KeysCalls == 1);
    CHECK_FALSE(r.addNewRows); // the legacy path never grows the grid past what a reload would hold
    REQUIRE(r.fetched.size() == 1);
    CHECK(r.fetched[0].id == "A-1");
    REQUIRE(r.verdicts.size() == 1);
    CHECK(r.verdicts[0].issueKey == "A-9");
    CHECK_FALSE(r.verdicts[0].stillExists); // the removal the capped roster could not make
}

TEST_CASE("Change monitor cycle: a complete roster never touches the legacy path and may add rows") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}}));
    const CycleResult r = RunCycle(fake, /*useRoster=*/true, {"A-1"});
    REQUIRE(r.ok);
    CHECK(r.addNewRows);
    CHECK(fake.ChangedSinceCalls == 0);
    CHECK(fake.KeysCalls == 0);
}

TEST_CASE("Change monitor cycle: a roster error skips the cycle without trying the legacy path") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRosterError(TrackerErrorTransport("down"));
    const CycleResult r = RunCycle(fake, /*useRoster=*/true, {"A-1"});
    CHECK_FALSE(r.ok);
    CHECK(fake.ChangedSinceCalls == 0);
}

TEST_CASE("Change monitor cycle: without the roster path only the legacy path runs") {
    LegacyPathClient fake;
    fake.ViewKeys = {"A-1"};
    const CycleResult r = RunCycle(fake, /*useRoster=*/false, {"A-1"});
    REQUIRE(r.ok);
    CHECK(fake.FetchRosterCallCount() == 0);
    CHECK(fake.ChangedSinceCalls == 1);
    CHECK_FALSE(r.addNewRows);
}

TEST_CASE("Change monitor cycle: a failed changed-since fetch after an incomplete roster skips the cycle") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}}, /*complete=*/false));
    fake.ChangedSinceFails = true;
    const CycleResult r = RunCycle(fake, /*useRoster=*/true, {"A-1"});
    CHECK_FALSE(r.ok);
    CHECK(fake.KeysCalls == 0);
}

TEST_CASE("Change monitor roster probe: a refused key does not starve the other changed rows") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T2"}, {"A-2", "T2"}, {"A-3", "T2"}}));
    fake.SetFetchIssuesForKeysResult(true, {Stamped("A-1", "T2"), Stamped("A-3", "T2")});
    fake.SetFetchIssuesForKeysRefusedKeys({"A-2"});
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}, {"A-2", "T1"}, {"A-3", "T1"}}, "pane",
                                           fetched, verdicts) == smatchet::sync::RosterProbeOutcome::Applied);
    CHECK(fetched.size() == 2);
}

TEST_CASE("Change monitor roster probe: vanished-key probes are capped per cycle") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({}));
    std::unordered_map<std::string, std::string> resident;
    for (int i = 0; i < 40; ++i) {
        resident["K-" + std::to_string(i)] = "T1";
    }
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, resident, "pane", fetched, verdicts) ==
            smatchet::sync::RosterProbeOutcome::Applied);
    CHECK(fake.Probes == 25);
    CHECK(verdicts.size() == 25);
}

TEST_CASE(
    "Change monitor roster probe: one refused key among 40 changed keys is skipped and every other body applies") {
    ProbeCountingClient fake;
    fake.SetRosterSupported(true);
    std::vector<std::pair<std::string, std::string>> entries;
    std::vector<CachedTicket> bodies;
    std::unordered_map<std::string, std::string> resident;
    for (int i = 0; i < 40; ++i) {
        const std::string id = "K-" + std::to_string(i);
        entries.emplace_back(id, "T2");
        bodies.push_back(Stamped(id, "T2"));
        resident[id] = "T1";
    }
    fake.SetRoster(MakeRoster(entries));
    fake.SetFetchIssuesForKeysResult(true, bodies);
    fake.SetFetchIssuesForKeysRefusedKeys({"K-27"});
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;

    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, resident, "pane", fetched, verdicts) ==
            smatchet::sync::RosterProbeOutcome::Applied);

    CHECK(fetched.size() == 39);
    for (const CachedTicket& t : fetched) {
        CHECK(t.id != "K-27");
    }
}

TEST_CASE("Change monitor roster probe: a key the tracker answers NotFound for is skipped, not a failed cycle") {
    struct NotFoundClient : ProbeCountingClient {
        Result<std::vector<CachedTicket>, TrackerError>
        FetchIssuesForKeysCancellable(const TrackerConfig& cfg, const std::vector<std::string>& keys,
                                      const ViewsStore& views, const CancelCallback& cancel) override {
            for (const std::string& k : keys) {
                if (k == "A-2") {
                    return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorNotFound("gone", 404));
                }
            }
            return ProbeCountingClient::FetchIssuesForKeysCancellable(cfg, keys, views, cancel);
        }
    };
    NotFoundClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T2"}, {"A-2", "T2"}}));
    fake.SetFetchIssuesForKeysResult(true, {Stamped("A-1", "T2"), Stamped("A-2", "T2")});
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}, {"A-2", "T1"}}, "pane", fetched,
                                           verdicts) == smatchet::sync::RosterProbeOutcome::Applied);
    REQUIRE(fetched.size() == 1);
    CHECK(fetched[0].id == "A-1");
}

TEST_CASE("Change monitor roster probe: rows the server returns beyond the requested keys are dropped") {
    struct HostileClient : ProbeCountingClient {
        Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeysCancellable(const TrackerConfig&,
                                                                                      const std::vector<std::string>&,
                                                                                      const ViewsStore&,
                                                                                      const CancelCallback&) override {
            // Asked for A-2 only; answers with extra rows and a duplicate.
            return Result<std::vector<CachedTicket>, TrackerError>::Ok(
                {Stamped("A-2", "T2"), Stamped("EVIL-1", "T9"), Stamped("A-1", "T9"), Stamped("A-2", "T2", "dup")});
        }
    };
    HostileClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}, {"A-2", "T2"}}));
    TrackerConfig cfg;
    ViewsStore views;
    std::vector<CachedTicket> fetched;
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    REQUIRE(smatchet::sync::RunRosterProbe(fake, cfg, views, {{"A-1", "T1"}, {"A-2", "T1"}}, "pane", fetched,
                                           verdicts) == smatchet::sync::RosterProbeOutcome::Applied);
    REQUIRE(fetched.size() == 1);
    CHECK(fetched[0].id == "A-2");
}

// --- delta-sync Phase 7: a remote change invalidates what hangs off the old version ---------------------------

TEST_CASE("A resident ticket whose tracker version moved is reported as remotely changed; others are not") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1"), Stamped("ABC-3", "T1")});
    rig.deps.ActiveTicketsImpl = {Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1"), Stamped("ABC-3", "T1")};
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}, {"ABC-3", "T1"}, {"ABC-4", "T1"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh"), Stamped("ABC-4", "T1", "new")});

    rig.Sync();
    REQUIRE(rig.Settle());

    // Only ABC-2 was on the grid with an older version: ABC-4 is new, ABC-1 and ABC-3 are unchanged.
    CHECK(rig.deps.RemotelyChanged == std::vector<std::string>{"ABC-2"});
}

TEST_CASE("An unchanged view reports nothing as remotely changed") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.deps.ActiveTicketsImpl = {Stamped("ABC-1", "T1")};
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.deps.RemotelyChanged.empty());
}

TEST_CASE(
    "A remote change invalidates under the pane's own tracker namespace for both transitions and edit permissions") {
    std::vector<std::pair<std::string, std::string>> transitions;
    std::vector<std::pair<std::string, std::string>> editMeta;
    smatchet::sync::InvalidateRemotelyChangedTickets(
        "Plane|ws", {"ABC-1", "ABC-2"},
        [&](const std::string& key, const std::string& id) { transitions.emplace_back(key, id); },
        [&](const std::string& key, const std::string& id) { editMeta.emplace_back(key, id); });
    const std::vector<std::pair<std::string, std::string>> expected = {{"Plane|ws", "ABC-1"}, {"Plane|ws", "ABC-2"}};
    CHECK(transitions == expected);
    CHECK(editMeta == expected);
}

// --- delta-sync review follow-ups: the change monitor's per-pane memory ---------------------------------------

namespace {
CycleResult RunCycleWithContext(LegacyPathClient& fake, bool useRoster, const std::vector<std::string>& cachedKeys,
                                const std::unordered_map<std::string, std::string>& resident,
                                const smatchet::sync::RosterProbeContext& context) {
    TrackerConfig cfg;
    ViewsStore views;
    CycleResult r;
    r.ok = smatchet::sync::RunChangeProbeCycle(fake, cfg, views, useRoster, resident, cachedKeys,
                                               std::chrono::seconds(120), {"summary"}, "pane", r.fetched, r.verdicts,
                                               r.addNewRows, context);
    return r;
}
} // namespace

TEST_CASE("Monitor pane state: an incomplete roster is skipped until the pane reloads") {
    smatchet::sync::ChangeMonitorPaneState state;
    CHECK(state.ShouldTryRoster(true, true));
    CHECK_FALSE(state.ShouldTryRoster(false, true)); // the feature switch still wins
    CHECK_FALSE(state.ShouldTryRoster(true, false)); // and so does a backend that cannot list versions

    smatchet::sync::ChangeProbeCycleNotes notes;
    notes.RosterIncomplete = true;
    notes.RefusedKeys = {"A-7"};
    state.Absorb(notes);
    CHECK_FALSE(state.ShouldTryRoster(true, true));
    CHECK(state.RefusedKeys.count("A-7") == 1);

    state.ResetForReload();
    CHECK(state.ShouldTryRoster(true, true));
    CHECK(state.RefusedKeys.empty());
}

TEST_CASE("Monitor pane state: the remembered refusals are bounded") {
    smatchet::sync::ChangeMonitorPaneState state;
    smatchet::sync::ChangeProbeCycleNotes notes;
    const std::size_t cap = smatchet::sync::ChangeMonitorPaneState::MaxRememberedRefusals();
    for (std::size_t i = 0; i < cap + 50; ++i) {
        notes.RefusedKeys.push_back("R-" + std::to_string(i));
    }
    state.Absorb(notes);
    CHECK(state.RefusedKeys.size() == cap);
}

TEST_CASE("Change monitor cycle: an incomplete roster is reported once, then the pane goes straight to changed-since") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}}, /*complete=*/false));
    fake.Changed = {Stamped("A-1", "T2")};
    fake.ViewKeys = {"A-1"};
    smatchet::sync::ChangeMonitorPaneState state;
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}};

    smatchet::sync::ChangeProbeCycleNotes notes;
    smatchet::sync::RosterProbeContext context;
    context.Notes = &notes;
    CycleResult first = RunCycleWithContext(fake, state.ShouldTryRoster(true, true), {"A-1"}, resident, context);
    REQUIRE(first.ok);
    CHECK(notes.RosterIncomplete);
    CHECK(fake.FetchRosterCallCount() == 1);
    state.Absorb(notes);

    // The apply hop recorded it: the next cycle skips the capped walk and still runs the legacy path.
    smatchet::sync::ChangeProbeCycleNotes secondNotes;
    context.Notes = &secondNotes;
    CycleResult second = RunCycleWithContext(fake, state.ShouldTryRoster(true, true), {"A-1"}, resident, context);
    REQUIRE(second.ok);
    CHECK(fake.FetchRosterCallCount() == 1); // no second roster walk
    CHECK(fake.ChangedSinceCalls == 2);
    CHECK_FALSE(secondNotes.RosterIncomplete);
}

TEST_CASE("Change monitor cycle: a backlog larger than one cycle keeps the anchor, a drained one advances it") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    const std::size_t total = smatchet::sync::kMaxMonitorBodiesPerCycle + 25;
    std::vector<std::pair<std::string, std::string>> listed;
    std::vector<CachedTicket> bodies;
    std::unordered_map<std::string, std::string> resident;
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < total; ++i) {
        const std::string key = "B-" + std::to_string(i);
        listed.emplace_back(key, "T2");
        bodies.push_back(Stamped(key, "T2"));
        resident[key] = "T1";
        keys.push_back(key);
    }
    fake.SetRoster(MakeRoster(listed));
    fake.SetFetchIssuesForKeysResult(true, bodies);

    smatchet::sync::ChangeProbeCycleNotes notes;
    smatchet::sync::RosterProbeContext context;
    context.Notes = &notes;
    const CycleResult capped = RunCycleWithContext(fake, true, keys, resident, context);
    REQUIRE(capped.ok);
    CHECK(capped.fetched.size() == smatchet::sync::kMaxMonitorBodiesPerCycle);
    CHECK(notes.BodiesCapped);
    CHECK_FALSE(smatchet::sync::ChangeMonitorPaneState::AnchorMayAdvance(notes));

    // The rows already merged now hold the new stamp: the remaining 25 fit in one cycle.
    for (std::size_t i = 0; i < smatchet::sync::kMaxMonitorBodiesPerCycle; ++i) {
        resident[keys[i]] = "T2";
    }
    smatchet::sync::ChangeProbeCycleNotes drainedNotes;
    context.Notes = &drainedNotes;
    const CycleResult drained = RunCycleWithContext(fake, true, keys, resident, context);
    REQUIRE(drained.ok);
    CHECK(drained.fetched.size() == 25);
    CHECK_FALSE(drainedNotes.BodiesCapped);
    CHECK(smatchet::sync::ChangeMonitorPaneState::AnchorMayAdvance(drainedNotes));
}

TEST_CASE("Change monitor cycle: a refused key is remembered and not asked for again") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T2"}, {"A-2", "T2"}, {"A-3", "T2"}}));
    fake.SetFetchIssuesForKeysResult(true, {Stamped("A-1", "T2"), Stamped("A-2", "T2"), Stamped("A-3", "T2")});
    fake.SetFetchIssuesForKeysRefusedKeys({"A-2"});
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}, {"A-2", "T1"}, {"A-3", "T1"}};
    const std::vector<std::string> keys = {"A-1", "A-2", "A-3"};

    smatchet::sync::ChangeProbeCycleNotes notes;
    smatchet::sync::RosterProbeContext context;
    context.Notes = &notes;
    const CycleResult first = RunCycleWithContext(fake, true, keys, resident, context);
    REQUIRE(first.ok);
    CHECK(first.fetched.size() == 2); // the readable rows still load
    CHECK(notes.RefusedKeys == std::vector<std::string>{"A-2"});
    const std::size_t callsAfterFirst = fake.FetchIssuesForKeysCallCount();
    CHECK(callsAfterFirst > 1); // isolating the refusal cost extra requests

    smatchet::sync::ChangeMonitorPaneState state;
    state.Absorb(notes);
    smatchet::sync::ChangeProbeCycleNotes secondNotes;
    context.Notes = &secondNotes;
    context.RefusedKeys = &state.RefusedKeys;
    const CycleResult second = RunCycleWithContext(fake, true, keys, resident, context);
    REQUIRE(second.ok);
    CHECK(second.fetched.size() == 2);
    CHECK(fake.FetchIssuesForKeysCallCount() == callsAfterFirst + 1); // one request: no isolation walk
    std::vector<std::string> asked = fake.FetchIssuesForKeysLastKeys();
    std::sort(asked.begin(), asked.end());
    CHECK(asked == std::vector<std::string>{"A-1", "A-3"});
    CHECK(secondNotes.RefusedKeys.empty());
}

TEST_CASE("Change monitor cycle: more refused keys than one cycle can isolate switch the pane to changed-since") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    std::vector<std::pair<std::string, std::string>> listed;
    std::vector<CachedTicket> bodies;
    std::unordered_map<std::string, std::string> resident;
    std::vector<std::string> keys;
    for (int i = 0; i < 40; ++i) {
        const std::string key = "K-" + std::to_string(i);
        listed.emplace_back(key, "T2");
        bodies.push_back(Stamped(key, "T2"));
        resident[key] = "T1";
        keys.push_back(key);
    }
    fake.SetRoster(MakeRoster(listed));
    fake.SetFetchIssuesForKeysResult(true, bodies);
    std::vector<std::string> refusedKeys;
    for (int i = 1; i < 40; i += 2) {
        refusedKeys.push_back("K-" + std::to_string(i)); // every other key: far more than the isolation budget
    }
    fake.SetFetchIssuesForKeysRefusedKeys(refusedKeys);
    fake.Changed = {Stamped("K-2", "T2")};
    fake.ViewKeys = keys;

    smatchet::sync::ChangeProbeCycleNotes notes;
    smatchet::sync::RosterProbeContext context;
    context.Notes = &notes;
    const CycleResult r = RunCycleWithContext(fake, true, keys, resident, context);

    REQUIRE(r.ok);
    CHECK(notes.RosterIncomplete); // the pane stops trying the keyed downloads until it reloads
    CHECK(fake.ChangedSinceCalls == 1);
    CHECK_FALSE(r.addNewRows);
}

TEST_CASE("Change monitor cycle: rows the hierarchy walk added are not probed as vanished") {
    LegacyPathClient fake;
    fake.SetRosterSupported(true);
    fake.SetRoster(MakeRoster({{"A-1", "T1"}})); // the view lists A-1 only
    fake.Exists = false;
    // A-9 is A-1's parent (added by the walk), A-8 is a row that really left the view.
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}, {"A-9", "T1"}, {"A-8", "T1"}};
    const std::unordered_set<std::string> owned = {"A-9"};
    smatchet::sync::RosterProbeContext context;
    context.WalkOwned = &owned;

    const CycleResult r = RunCycleWithContext(fake, true, {"A-1", "A-9", "A-8"}, resident, context);

    REQUIRE(r.ok);
    REQUIRE(r.verdicts.size() == 1);
    CHECK(r.verdicts[0].issueKey == "A-8");
    CHECK(fake.Probes == 1);
}

TEST_CASE("WalkOwnedKeys without a view listing names only the parents a pane holds beside their children") {
    CachedTicket epic = MakeTicket("E-1", "epic");
    CachedTicket story = MakeTicket("S-1", "story");
    story.fieldValues["parent"] = "E-1 - epic";
    CachedTicket orphan = MakeTicket("O-1", "no relatives");
    CachedTicket away = MakeTicket("T-1", "parent not held");
    away.fieldValues["parent"] = "X-1 - elsewhere";

    const std::unordered_set<std::string> owned =
        smatchet::sync::WalkOwnedKeys({epic, story, orphan, away}, nullptr, &ParentHierarchyPure::ParentKeyOf);

    CHECK(owned == std::unordered_set<std::string>{"E-1"}); // a child may be a view row that left: it stays checkable
}

TEST_CASE("WalkOwnedKeys with a view listing also owns every row the view's query did not return") {
    CachedTicket epic = MakeTicket("E-1", "epic");
    CachedTicket story = MakeTicket("S-1", "story"); // listed by the view, child of the epic
    story.fieldValues["parent"] = "E-1 - epic";
    CachedTicket walkChild = MakeTicket("S-2", "walk child"); // loaded by the children walk
    walkChild.fieldValues["parent"] = "S-1 - story";
    CachedTicket gone = MakeTicket("G-1", "left the view"); // listed at the last reload, still resident
    const std::unordered_set<std::string> listed = {"S-1", "G-1"};

    const std::unordered_set<std::string> owned =
        smatchet::sync::WalkOwnedKeys({epic, story, walkChild, gone}, &listed, &ParentHierarchyPure::ParentKeyOf);

    // E-1 is a parent of a resident row and unlisted; S-2 is unlisted; S-1 is also a parent. G-1 is the one row
    // the monitor may still report as having left the view.
    CHECK(owned == std::unordered_set<std::string>{"E-1", "S-1", "S-2"});
}

TEST_CASE("A completed delta reload publishes the view's own ids, not the hierarchy walk's") {
    DeltaRig rig;
    CachedTicket child = Stamped("ABC-1", "T1");
    child.fieldValues["parent"] = "ABC-9 - epic";
    rig.Seed({child, Stamped("ABC-9", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}})); // the view lists the child only; the epic is the walk's
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-9", "T1")});

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.deps.ViewListedPublishes == 1);
    CHECK(rig.deps.ViewListed == std::vector<std::string>{"ABC-1"});
}

TEST_CASE("A full reload publishes the ids its query streamed") {
    DeltaRig rig;
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")}, true);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(rig.Settle());

    CHECK(rig.deps.ViewListedPublishes == 1);
    std::vector<std::string> listed = rig.deps.ViewListed;
    std::sort(listed.begin(), listed.end());
    CHECK(listed == std::vector<std::string>{"ABC-1", "ABC-2"});
}

TEST_CASE("A reload that did not complete publishes no view listing") {
    DeltaRig rig;
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, /*fullSyncCompleted=*/false);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(rig.Settle());

    CHECK(rig.deps.ViewListedPublishes == 0);
}

// --- delta-sync review follow-ups: version rendering and the pane's own cached versions -----------------------

TEST_CASE("Delta reload: a pane with none of its own cached versions goes straight to a full fetch") {
    DeltaRig rig;
    rig.Seed({Stamped("OTHER-1", "T1"), Stamped("OTHER-2", "T1")}); // a sibling pane's rows share the namespace
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());

    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "full");
    CHECK(stats.FallbackReason.find("no cached ticket versions for this view") != std::string::npos);
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0); // nothing was downloaded as "new" by key
    CHECK(rig.fake->FetchIssuesCallCount() == 1);        // the streamed full fetch ran instead
}

TEST_CASE("Delta reload: a stamp re-rendered in another zone is not a change") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "2024-05-01T10:00:00.000+0000")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "2024-05-01T12:00:00.000+0200"}}));

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0);
    const smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "delta");
    CHECK(stats.BodiesFetched == 0);
    CHECK(stats.Unchanged == 1);
}

// --- delta-sync review follow-ups: the apply drain, rows the cache holds newer, refused keys ------------------

TEST_CASE("The apply drain builds its id map once per session, not once per slice") {
    DeltaRig rig;
    std::vector<CachedTicket> rows;
    for (int i = 0; i < 100; ++i) {
        rows.push_back(Stamped("K-" + std::to_string(i), "T1"));
    }
    rig.fake->SetFetchIssuesResult(rows, true);

    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(rig.Settle());

    CHECK(rig.deps.ActiveTicketsImpl.size() == 100);
    // 100 fetched rows apply 20 per frame: five slices, one map.
    CHECK(rig.svc.MergeIndexBuilds() == 1);
}

TEST_CASE("The apply drain rebuilds its id map when another path replaced the roster during the session") {
    DeltaRig rig;
    std::vector<CachedTicket> rows;
    for (int i = 0; i < 40; ++i) {
        rows.push_back(Stamped("K-" + std::to_string(i), "T1"));
    }
    rig.fake->SetFetchIssuesResult(rows, true);
    rig.Sync(smatchet::sync::SyncMode::Full);
    REQUIRE(SpinUntil(rig.svc, [&]() { return rig.svc.MergeIndexBuilds() >= 1; }));
    const std::size_t buildsBefore = rig.svc.MergeIndexBuilds();

    {
        // A refresh from the local cache swaps the roster wholesale (and announces it with a revision bump).
        std::lock_guard<std::mutex> lock(rig.deps.ActiveTicketsMutexImpl);
        rig.deps.ActiveTicketsImpl.clear();
        rig.deps.ActiveTicketsImpl.push_back(Stamped("K-39", "T1", "from the cache"));
        rig.deps.BumpActiveTicketsRevision();
    }
    REQUIRE(rig.Settle());

    CHECK(rig.svc.MergeIndexBuilds() == buildsBefore + 1); // the next slice saw the announced change and rebuilt
    // No duplicate ids whatever the interleaving.
    std::unordered_set<std::string> seen;
    for (const CachedTicket& row : rig.deps.ActiveTicketsImpl) {
        CHECK(seen.insert(row.id).second);
    }
}

TEST_CASE("A fetched row the cache holds newer reaches a pane that did not hold it") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "2024-05-02T10:00:00.000+0000", "cached newer")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "2024-05-01T10:00:00.000+0000"}})); // the roster is behind the cache
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-1", "2024-05-01T10:00:00.000+0000", "fetched older")});
    REQUIRE(rig.deps.ActiveTicketsImpl.empty()); // a fresh pane

    rig.Sync();
    REQUIRE(rig.Settle());

    // The older fetched copy was not persisted ...
    CachedTicket stored;
    REQUIRE(rig.deps.CacheImpl->TryGetTicket("Jira", "ABC-1", stored));
    CHECK(stored.fieldValues["summary"] == "cached newer");
    // ... and the pane shows the cached newer copy instead of lacking the row.
    REQUIRE(rig.deps.ActiveTicketsImpl.size() == 1);
    CHECK(rig.deps.ActiveTicketsImpl[0].id == "ABC-1");
    CHECK(rig.deps.ActiveTicketsImpl[0].fieldValues["summary"] == "cached newer");
}

TEST_CASE("A row served from the cache at a newer version than the resident one reports the remote change") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T2", "cached at T2")});
    rig.deps.ActiveTicketsImpl = {Stamped("ABC-1", "T1", "resident at T1")}; // the pane still holds the older copy
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T2"}})); // the cache is current: nothing to download

    rig.Sync();
    REQUIRE(rig.Settle());

    CHECK(rig.fake->FetchIssuesForKeysCallCount() == 0);
    REQUIRE(rig.deps.ActiveTicketsImpl.size() == 1);
    CHECK(rig.deps.ActiveTicketsImpl[0].fieldValues["summary"] == "cached at T2");
    CHECK(rig.deps.RemotelyChanged == std::vector<std::string>{"ABC-1"}); // transitions and edit permissions go stale
}

TEST_CASE("Delta reload: a key the tracker refused is remembered, so later attempts stay delta until a full sync") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh")});
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-2"});
    // The streamed fetch still serves the row, at a stamp behind the roster's: the key stays "changed" next time.
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1", "from full")}, true);

    rig.Sync(); // first attempt: refused, falls back to the full fetch
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK(rig.svc.LastSessionStats().FallbackReason.find("ABC-2") != std::string::npos);
    const std::size_t keyedCallsAfterFirst = rig.fake->FetchIssuesForKeysCallCount();

    rig.Sync(); // second attempt: the key is skipped, the reload stays delta
    REQUIRE(rig.Settle());
    smatchet::sync::SyncSessionStats stats = rig.svc.LastSessionStats();
    CHECK(stats.Mode == "delta");
    CHECK(stats.FallbackReason.empty());
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == keyedCallsAfterFirst); // it was not asked for again
    CHECK(rig.CachedCount() == 2);                                          // nothing purged on the way
    CHECK(stats.Completed); // its cached row stands in: the session is still authoritative
    CHECK(rig.deps.LastTrackerTicketSyncWarning.find("listed by the tracker") == std::string::npos);
    bool paneHoldsRefused = false;
    for (const CachedTicket& row : rig.deps.ActiveTicketsImpl) {
        paneHoldsRefused = paneHoldsRefused || row.id == "ABC-2";
    }
    CHECK(paneHoldsRefused);

    rig.Sync(smatchet::sync::SyncMode::Full); // an explicit full sync forgets the memory
    REQUIRE(rig.Settle());
    rig.Sync();
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "full"); // asked again, refused again, fell back again
    CHECK(rig.fake->FetchIssuesForKeysCallCount() > keyedCallsAfterFirst);
}

TEST_CASE("Delta reload: a remembered refusal is forgotten once the fallback full fetch serves the key's body") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-2"});
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh")});
    // The streamed fetch serves the key's body at the version the roster listed (the one it was refused for).
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T2", "from full")}, true);

    rig.Sync(); // refused, falls back to the full fetch, which serves the body
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK(rig.svc.LastSessionStats().FallbackReason.find("ABC-2") != std::string::npos);

    // The keyed endpoint works again and the ticket moves on: the key must be asked for, not skipped as refused.
    rig.fake->SetFetchIssuesForKeysRefusedKeys({});
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T3", "newest")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T3"}}));
    const std::size_t keyedBefore = rig.fake->FetchIssuesForKeysCallCount();
    rig.Sync();
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == keyedBefore + 1);
    CHECK(rig.fake->FetchIssuesForKeysLastKeys() == std::vector<std::string>{"ABC-2"});
}

TEST_CASE("Delta reload: a remembered refusal expires after DeltaSyncMaxRowAgeSec and is tried again") {
    DeltaRig rig;
    rig.cfg.DeltaSyncMaxRowAgeSec = 2;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T2"}}));
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-2"});
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", "T2", "fresh")});
    // The streamed fetch lags the roster (T1 < T2), so the refusal stays remembered after the fallback.
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1", "from full")}, true);

    rig.Sync();
    REQUIRE(rig.Settle());
    const std::size_t keyedAfterFirst = rig.fake->FetchIssuesForKeysCallCount();
    rig.Sync(); // within the window: skipped, no keyed call
    REQUIRE(rig.Settle());
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == keyedAfterFirst);

    rig.deps.ClockOffsetSec += 3; // past DeltaSyncMaxRowAgeSec: the injected clock moves, nothing sleeps
    rig.Sync();
    REQUIRE(rig.Settle());
    CHECK(rig.fake->FetchIssuesForKeysCallCount() > keyedAfterFirst); // the refusal was retried
    std::vector<std::string> asked = rig.fake->FetchIssuesForKeysLastKeys();
    CHECK(std::find(asked.begin(), asked.end(), "ABC-2") != asked.end());
}

TEST_CASE("Delta reload: a refused key whose roster version is newer than the refusal is asked for again") {
    // Real instants: a refusal is only comparable to a roster version that parses.
    const std::string t1 = "2026-05-01T10:00:00.000+0000";
    const std::string t2 = "2026-05-01T11:00:00.000+0000";
    const std::string t3 = "2026-05-01T12:00:00.000+0000";
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", t1), Stamped("ABC-2", t1)});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", t1}, {"ABC-2", t2}}));
    rig.fake->SetFetchIssuesForKeysRefusedKeys({"ABC-2"});
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", t2, "fresh")});
    // The streamed fetch lags the roster, so the refusal (remembered at t2) stays after the fallback.
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", t1), Stamped("ABC-2", t1, "from full")}, true);
    rig.Sync();
    REQUIRE(rig.Settle());
    const std::size_t keyedAfterFirst = rig.fake->FetchIssuesForKeysCallCount();

    rig.Sync(); // the roster still lists t2: the refusal stands, no keyed call
    REQUIRE(rig.Settle());
    CHECK(rig.fake->FetchIssuesForKeysCallCount() == keyedAfterFirst);

    rig.fake->SetRoster(MakeRoster({{"ABC-1", t1}, {"ABC-2", t3}})); // ABC-2 was edited since the refusal
    rig.fake->SetFetchIssuesForKeysRefusedKeys({});
    rig.fake->SetFetchIssuesForKeysResult(true, {Stamped("ABC-2", t3, "edited")});
    rig.Sync();
    REQUIRE(rig.Settle());
    CHECK(rig.fake->FetchIssuesForKeysCallCount() > keyedAfterFirst); // asked again, not skipped
    const std::vector<std::string> asked = rig.fake->FetchIssuesForKeysLastKeys();
    CHECK(std::find(asked.begin(), asked.end(), "ABC-2") != asked.end());
}

TEST_CASE("TicketSyncService: a saved-views read in flight keeps the pane from retiring") {
    // The read's worker posts through the deps the pane's context owns; retiring the pane frees them, so
    // AppController::retireExpiredHiddenContexts_ postpones while HasPendingWork() is true.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult({MakeTicket("LATE-1", "late")}, /*fullSyncCompleted=*/true);
    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    CHECK_FALSE(svc.HasPendingWork());

    svc.SyncWithBackend(&cfg, nullptr);
    CHECK(svc.HasPendingWork());
    CHECK_FALSE(svc.IsActive()); // no sync is running: the status line must not claim one

    deps.RunDeferredTasks(); // the read finishes and posts the start
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    CHECK_FALSE(svc.HasPendingWork());
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a saved-views read that outlives its service never touches the deps") {
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    {
        TicketSyncService svc(deps);
        TrackerConfig cfg;
        cfg.TrackerType = "fake";
        svc.SyncWithBackend(&cfg, nullptr);
        REQUIRE(svc.HasPendingWork());
    } // retired with the read still queued
    deps.RunDeferredTasks();
    CHECK(deps.PostsToMainThread == 0); // the deps are the pane context's, possibly gone by now: not reached
    CHECK(deps.ActiveTicketsImpl.empty());
}

TEST_CASE("TicketSyncService cache teardown hands the worker out, holds every start back, and lifts on End" *
          doctest::test_suite("[high-risk]")) {
    // cache.recreate runs on a worker: BeginCacheTeardown must flag the running sync cancelled, hand its thread to the
    // caller to join OFF the UI thread, and block new starts (SyncWithBackend drops, TickStreamingApply idles) so
    // nothing touches the cache until EndCacheTeardown.
    constexpr std::chrono::milliseconds kPageDelay(100);
    FakeTicketSyncDeps deps;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetSlowStreamedPages(/*pages=*/30, kPageDelay);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    bool inFlight = false;
    for (int i = 0; i < 1000 && !inFlight; ++i) {
        inFlight = fake->SlowStreamedPagesStarted() >= 1;
        if (!inFlight) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(inFlight);
    REQUIRE(svc.IsActive());

    std::thread handedOut = svc.BeginCacheTeardown();
    CHECK(handedOut.joinable()); // the caller joins it: the service no longer owns the thread
    CHECK(svc.IsCacheTeardownActive());
    const int pagesAtTeardown = fake->SlowStreamedPagesStarted();

    // A start request during the teardown does not start (it is recorded and replayed at End), and the apply tick
    // does nothing.
    svc.SyncWithBackend(&cfg, &views);
    svc.TickStreamingApply();

    const auto joinStart = std::chrono::steady_clock::now();
    handedOut.join();
    const auto joinElapsed = std::chrono::steady_clock::now() - joinStart;
    CHECK(joinElapsed < kPageDelay * 10); // cancelled at the next page boundary, not 30 pages later
    CHECK(fake->SlowStreamedPagesStarted() <= pagesAtTeardown + 1);

    svc.EndCacheTeardown();
    CHECK_FALSE(svc.IsCacheTeardownActive());
    // The request made during the teardown is replayed now, not lost (it was a slow 30-page fetch: cancel it).
    CHECK(svc.IsActive());
    svc.CancelAndJoinActiveStreamingSync();
    CHECK_FALSE(svc.IsActive());

    // Syncs start again after the gate lifts.
    fake->SetSlowStreamedPages(0, std::chrono::milliseconds(0));
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("AFTER-1", "after"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);
    svc.SyncWithBackend(&cfg, &views);
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService cache teardown with no running sync hands out nothing and still blocks starts") {
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);
    std::thread handedOut = svc.BeginCacheTeardown();
    CHECK_FALSE(handedOut.joinable());
    CHECK(svc.IsCacheTeardownActive());
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);
    CHECK_FALSE(svc.IsActive()); // dropped, not started
    svc.EndCacheTeardown();
    CHECK_FALSE(svc.IsCacheTeardownActive());
}

TEST_CASE("TicketSyncService: a sync requested during the cache teardown runs once the teardown ends") {
    // A pane opened mid-recreate kicks its one-shot first sync while the gate is up; dropping it left the pane
    // empty forever (its kick latch was already set). The request is recorded and EndCacheTeardown replays it.
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1"), Stamped("ABC-2", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}, {"ABC-2", "T1"}}));

    (void)rig.svc.BeginCacheTeardown();
    rig.Sync();
    CHECK_FALSE(rig.svc.IsActive()); // held back: nothing started, nothing touched the cache
    rig.svc.TickStreamingApply();
    CHECK(rig.fake->FetchRosterCallCount() == 0);
    CHECK(rig.deps.ActiveTicketsImpl.empty());

    rig.svc.EndCacheTeardown();
    REQUIRE(rig.Settle());
    CHECK(rig.fake->FetchRosterCallCount() == 1); // the held-back request ran
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.deps.ActiveTicketsImpl.size() == 2);
}

TEST_CASE("TicketSyncService: a Full sync held back during the cache teardown is still Full when it runs") {
    // Full is sticky: a later ordinary refresh in the same window does not downgrade it, in either order.
    for (const bool fullFirst : {true, false}) {
        CAPTURE(fullFirst);
        DeltaRig rig;
        rig.Seed({Stamped("ABC-1", "T1")});
        rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
        rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);

        (void)rig.svc.BeginCacheTeardown();
        rig.Sync(fullFirst ? smatchet::sync::SyncMode::Full : smatchet::sync::SyncMode::Delta);
        rig.Sync(fullFirst ? smatchet::sync::SyncMode::Delta : smatchet::sync::SyncMode::Full);
        CHECK(rig.fake->FetchIssuesCallCount() == 0);

        rig.svc.EndCacheTeardown();
        REQUIRE(rig.Settle());
        CHECK(rig.svc.LastSessionStats().Mode == "full");
        CHECK(rig.fake->FetchIssuesCallCount() == 1); // the whole view was downloaded
        CHECK(rig.fake->FetchRosterCallCount() == 0);
    }
}

TEST_CASE("TicketSyncService: an ordinary sync held back during the cache teardown stays a delta") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    (void)rig.svc.BeginCacheTeardown();
    rig.Sync();
    rig.Sync();
    rig.svc.EndCacheTeardown();
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.fake->FetchIssuesCallCount() == 0);
    CHECK(rig.fake->FetchRosterCallCount() == 1); // two held-back requests replay as one
}

TEST_CASE("TicketSyncService: a views load that lands during the cache teardown is replayed, not lost") {
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("LATE-1", "late"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    svc.SyncWithBackend(&cfg, nullptr); // the views file is being read on a worker
    (void)svc.BeginCacheTeardown();     // the recreate starts meanwhile
    deps.RunDeferredTasks();            // the read finishes: its post tries to start the sync while the gate is up
    CHECK_FALSE(svc.IsActive());

    svc.EndCacheTeardown();
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a request held during the recreate rides on the follow-up sync, one Full session runs") {
    // FinishCacheRecreate_ folds the focused pane's held request into onDone's follow-up (EndCacheTeardown(true)):
    // replaying it first made the follow-up cancel and restart it (a double download, or a replayed Delta cancelling
    // the Full). Both orders of the two modes end in exactly one Full session.
    for (const bool heldIsFull : {false, true}) {
        CAPTURE(heldIsFull);
        DeltaRig rig;
        rig.Seed({Stamped("ABC-1", "T1")});
        rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
        rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);

        const std::uint64_t epochBefore = rig.svc.SessionEpoch();
        (void)rig.svc.BeginCacheTeardown();
        rig.Sync(heldIsFull ? smatchet::sync::SyncMode::Full : smatchet::sync::SyncMode::Delta);
        rig.svc.EndCacheTeardown(/*foldIntoFollowUp=*/true);
        CHECK(rig.svc.SessionEpoch() == epochBefore); // folded: the held request did not start a session
        CHECK_FALSE(rig.svc.IsActive());

        rig.Sync(heldIsFull ? smatchet::sync::SyncMode::Delta : smatchet::sync::SyncMode::Full); // onDone's follow-up
        REQUIRE(rig.Settle());
        CHECK(rig.svc.SessionEpoch() == epochBefore + 1); // exactly one session
        CHECK(rig.svc.LastSessionStats().Mode == "full");
        CHECK(rig.fake->FetchIssuesCallCount() == 1); // the view was downloaded once
        CHECK(rig.fake->FetchRosterCallCount() == 0);
    }
}

TEST_CASE("TicketSyncService: a fold nobody consumed does not turn a later refresh into a Full") {
    // EndCacheTeardown(true) keeps the held request's mode for the follow-up sync. If that follow-up never ran, the
    // next recreate's BeginCacheTeardown drops the fold, so an ordinary refresh afterwards stays a Delta.
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1")}, /*fullSyncCompleted=*/true);

    (void)rig.svc.BeginCacheTeardown();
    rig.Sync(smatchet::sync::SyncMode::Full); // held by the gate
    rig.svc.EndCacheTeardown(/*foldIntoFollowUp=*/true);
    // ...and nothing syncs. A second recreate starts and ends without a held request.
    (void)rig.svc.BeginCacheTeardown();
    rig.svc.EndCacheTeardown();

    rig.Sync(smatchet::sync::SyncMode::Delta);
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.fake->FetchIssuesCallCount() == 0);
}

TEST_CASE("TicketSyncService: EndCacheTeardown without a raised gate replays nothing and cancels nothing") {
    // FinishCacheRecreate_ lists the focused service twice; the second End must not cancel a session the first one
    // replayed.
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    (void)rig.svc.BeginCacheTeardown();
    rig.Sync();
    rig.svc.EndCacheTeardown();
    const std::uint64_t epochAfterReplay = rig.svc.SessionEpoch();
    rig.svc.EndCacheTeardown(); // the duplicate
    CHECK(rig.svc.SessionEpoch() == epochAfterReplay);
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Completed);
}

TEST_CASE("TicketSyncService::SyncWithBackend without views reads them on a worker, then starts the sync") {
    // Pillar 2: the saved views file is read and parsed off the UI thread; the sync starts from the UI-thread post.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    std::vector<CachedTicket> scripted;
    scripted.push_back(MakeTicket("LATE-1", "late"));
    fake->SetFetchIssuesResult(scripted, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);
    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    svc.SyncWithBackend(&cfg, nullptr);

    CHECK(deps.BackgroundTasksLaunched == 1);
    CHECK_FALSE(svc.IsActive()); // nothing started on the calling thread

    deps.RunDeferredTasks(); // the worker reads the views, the post starts the sync
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.ActiveTicketsImpl.size() == 1; }));
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService::SyncWithBackend without views drops the start when the service is gone") {
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    {
        TicketSyncService svc(deps);
        TrackerConfig cfg;
        cfg.TrackerType = "fake";
        svc.SyncWithBackend(&cfg, nullptr);
        CHECK(deps.BackgroundTasksLaunched == 1);
    } // a pane retired while its views were being read
    deps.RunDeferredTasks(); // the post finds the service gone and does nothing
    CHECK(deps.ActiveTicketsImpl.empty());
}

namespace {

// A Full session parked inside a slow fetch, with a second request queued behind it (Full is sticky, so the queued
// request is a Full too), as a pane that was busy when a cache recreate began. Returns once the first fetch is
// on the wire; the epoch is read after the first session started.
void ParkRunningFullWithQueuedDelta(DeltaRig& rig) {
    rig.fake->SetSlowStreamedPages(/*pages=*/30, std::chrono::milliseconds(50));
    rig.Sync(smatchet::sync::SyncMode::Full);
    bool onTheWire = false;
    for (int i = 0; i < 1000 && !onTheWire; ++i) {
        onTheWire = rig.fake->SlowStreamedPagesStarted() >= 1;
        if (!onTheWire) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(onTheWire);
    rig.Sync(smatchet::sync::SyncMode::Delta); // queued behind the running session
}

} // namespace

TEST_CASE("TicketSyncService: a sync queued before the recreate rides on the follow-up sync, one Full session runs") {
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);
    ParkRunningFullWithQueuedDelta(rig);
    const std::uint64_t epochBefore = rig.svc.SessionEpoch();

    std::thread handedOut = rig.svc.BeginCacheTeardown();
    if (handedOut.joinable()) {
        handedOut.join();
    }
    rig.fake->SetSlowStreamedPages(0, std::chrono::milliseconds(0));
    rig.svc.EndCacheTeardown(/*foldIntoFollowUp=*/true);
    CHECK(rig.svc.SessionEpoch() == epochBefore); // folded: the queued request started nothing of its own
    CHECK_FALSE(rig.svc.IsActive());

    rig.Sync(smatchet::sync::SyncMode::Full); // FinishCacheRecreate_'s follow-up
    REQUIRE(rig.Settle());
    for (int i = 0; i < 20; ++i) { // a leftover queued request would start its own session on a later tick
        rig.svc.TickStreamingApply();
    }
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == epochBefore + 1); // exactly one session
    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK(rig.fake->FetchIssuesCallCount() == 1);
}

TEST_CASE("TicketSyncService: a sync queued before the recreate is replayed once on a pane with no follow-up") {
    // A non-focused pane: EndCacheTeardown replays the request, so the pane is syncing (HasPendingWork) and
    // RefreshOtherPanesAfterCacheWipe_ does not re-arm a second sync on top of it.
    DeltaRig rig;
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);
    ParkRunningFullWithQueuedDelta(rig);
    const std::uint64_t epochBefore = rig.svc.SessionEpoch();

    std::thread handedOut = rig.svc.BeginCacheTeardown();
    if (handedOut.joinable()) {
        handedOut.join();
    }
    rig.fake->SetSlowStreamedPages(0, std::chrono::milliseconds(0));
    rig.svc.EndCacheTeardown();
    CHECK(rig.svc.HasPendingWork()); // the replay is running: the re-arm guard sees it

    REQUIRE(rig.Settle());
    for (int i = 0; i < 20; ++i) {
        rig.svc.TickStreamingApply();
    }
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == epochBefore + 1); // exactly one session
    CHECK(rig.svc.LastSessionStats().Mode == "full");
    CHECK(rig.fake->FetchIssuesCallCount() == 1);
}

TEST_CASE("TicketSyncService::SyncWithBackend without a config reads it on a worker, not on the calling thread") {
    // The recreate's follow-up and sync.full pass no config: parsing the file on the UI thread cost a frame.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* factory = static_cast<smatchet_tests::FakeTrackerBackendFactory*>(deps.Factory.get());

    TrackerConfig onDiskAtCall;
    onDiskAtCall.TrackerType = "GitHub";
    ConfigManager::Save(onDiskAtCall);

    TicketSyncService svc(deps);
    svc.SyncWithBackend(nullptr, nullptr, smatchet::sync::SyncMode::Full);
    CHECK(deps.BackgroundTasksLaunched == 1);
    CHECK(svc.HasPendingWork());

    // The file changes while the worker has not run yet: only a read on the worker sees this.
    TrackerConfig onDiskAtRun;
    onDiskAtRun.TrackerType = "Plane";
    ConfigManager::Save(onDiskAtRun);

    deps.RunDeferredTasks();
    REQUIRE_FALSE(factory->CreatedTypes.empty());
    CHECK(factory->CreatedTypes.front() == "Plane");
    svc.CancelAndJoinActiveStreamingSync();
}

namespace {

/// Two panes' worth of config, told apart by the tracker kind the sync builds a client for.
TrackerConfig ConfigFor(const char* trackerType) {
    TrackerConfig cfg;
    cfg.TrackerType = trackerType;
    return cfg;
}

/// Runs the parked worker tasks (each reads its files and posts the start), leaving the posts parked.
void ReadFilesLeavePostsParked(FakeTicketSyncDeps& deps) {
    deps.DeferPosts = true;
    deps.RunDeferredTasks();
}

/// A cached view the tracker reports unchanged: a Delta sync runs as delta, a Full one as full.
void SeedUnchangedView(DeltaRig& rig) {
    rig.Seed({Stamped("ABC-1", "T1")});
    rig.fake->SetRoster(MakeRoster({{"ABC-1", "T1"}}));
    rig.fake->SetFetchIssuesResult({Stamped("ABC-1", "T1", "from full")}, /*fullSyncCompleted=*/true);
}

} // namespace

TEST_CASE("TicketSyncService: two async requests whose posts land in reverse order, the newer one wins") {
    // The files reads run on separate threads, so the older request's post can land after the newer one's: applying it
    // would supersede the newer session and sync the previous view.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* factory = static_cast<smatchet_tests::FakeTrackerBackendFactory*>(deps.Factory.get());
    TicketSyncService svc(deps);
    const TrackerConfig older = ConfigFor("GitHub");
    const TrackerConfig newer = ConfigFor("Plane");
    svc.SyncWithBackend(&older, nullptr);
    svc.SyncWithBackend(&newer, nullptr);
    ReadFilesLeavePostsParked(deps);
    REQUIRE(deps.DeferredPosts.size() == 2);

    deps.RunDeferredPosts(/*reverse=*/true); // the newer post lands first
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.SessionEpoch() == 1; }));
    REQUIRE(factory->CreatedTypes.size() == 1);
    CHECK(factory->CreatedTypes.front() == "Plane"); // the older request did not supersede it with a GitHub client
    CHECK(svc.SessionEpoch() == 1);
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: an async request followed by a synchronous one, the synchronous one wins") {
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* factory = static_cast<smatchet_tests::FakeTrackerBackendFactory*>(deps.Factory.get());
    TicketSyncService svc(deps);
    const TrackerConfig older = ConfigFor("GitHub");
    const TrackerConfig newer = ConfigFor("Plane");
    ViewsStore views;
    svc.SyncWithBackend(&older, nullptr); // still reading its files
    svc.SyncWithBackend(&newer, &views);  // starts at once
    REQUIRE(factory->CreatedTypes.size() == 1);
    CHECK(factory->CreatedTypes.front() == "Plane");

    deps.RunDeferredTasks(); // the older read finishes and posts: it finds itself overtaken
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(factory->CreatedTypes.size() == 1);
    CHECK(svc.SessionEpoch() == 1);
    CHECK_FALSE(svc.HasPendingWork());
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a dropped async Full is kept, so the request that overtook it runs as Full") {
    DeltaRig rig;
    SeedUnchangedView(rig); // a Delta would run as delta here, so the mode shows which request won
    rig.deps.DeferBackgroundTasks = true;
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Full);  // older
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Delta); // newer
    ReadFilesLeavePostsParked(rig.deps);
    REQUIRE(rig.deps.DeferredPosts.size() == 2);

    rig.deps.RunDeferredPosts(); // the older post lands first and is dropped; its Full rides on the newer request
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == 1);
    CHECK(rig.svc.LastSessionStats().Mode == "full");
}

TEST_CASE("TicketSyncService: a Full dropped after the newer request already started is kept for the next one") {
    DeltaRig rig;
    SeedUnchangedView(rig);
    rig.deps.DeferBackgroundTasks = true;
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Full);  // older
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Delta); // newer
    ReadFilesLeavePostsParked(rig.deps);

    rig.deps.RunDeferredPosts(/*reverse=*/true); // the Delta starts first; the Full then finds itself overtaken
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "delta");
    CHECK(rig.svc.SessionEpoch() == 1);

    rig.deps.DeferBackgroundTasks = false;
    rig.deps.DeferPosts = false;
    rig.Sync(smatchet::sync::SyncMode::Delta); // the next request carries the Full that was dropped
    REQUIRE(rig.Settle());
    CHECK(rig.svc.LastSessionStats().Mode == "full");
}

TEST_CASE("TicketSyncService::SyncWithBackend with views but no config reads the config on a worker") {
    // The shape view.activate, view.refresh_active and view.create pass: (nullptr, &views).
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    auto* factory = static_cast<smatchet_tests::FakeTrackerBackendFactory*>(deps.Factory.get());
    ConfigManager::Save(ConfigFor("Plane"));

    TicketSyncService svc(deps);
    ViewsStore views;
    svc.SyncWithBackend(nullptr, &views);
    CHECK(deps.BackgroundTasksLaunched == 1); // the config is read off the calling thread
    CHECK(factory->CreatedTypes.empty());
    CHECK(svc.HasPendingWork());
    CHECK_FALSE(svc.IsActive());

    deps.RunDeferredTasks();
    REQUIRE(factory->CreatedTypes.size() == 1);
    CHECK(factory->CreatedTypes.front() == "Plane"); // built from the file's config
    CHECK(SpinUntil(svc, [&]() { return !svc.IsActive() && svc.SessionEpoch() == 1; }));
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a sync that needs a new backend does not start when there is no factory") {
    // A controller that was never Initialize()d has no backend factory; the sync must not dereference null, and it
    // must not run against the old tracker either (its issues would be cached under the new tracker's key).
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.Factory.reset();
    ITrackerBackend* const before = deps.BackendImpl.get();
    TicketSyncService svc(deps);
    const TrackerConfig plane = ConfigFor("Plane");
    ViewsStore views;
    svc.SyncWithBackend(&plane, &views);
    CHECK(deps.BackendImpl.get() == before);
    CHECK(svc.SessionEpoch() == 0); // no session was started
    CHECK_FALSE(svc.IsActive());
    CHECK_FALSE(svc.HasPendingWork());
    CHECK(deps.SyncNotifications.empty()); // no "Syncing" toast for a sync that never ran
    // The status surface (sync.tracker_status reads the sync warning) says why.
    CHECK(deps.LastTrackerTicketSyncWarning.find("Sync not started") != std::string::npos);
    CHECK(deps.LastTrackerTicketSyncWarning.find("Plane") != std::string::npos);
    CHECK_FALSE(deps.LastTrackerTicketSyncWarningTransient);
    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a views read whose task is skipped or whose launch throws leaves nothing pending") {
    // The count of reads in flight is carried by a guard the task owns: a task dropped at shutdown (the real
    // LaunchBackgroundTask returns without running it) or a launch that throws must not leave HasPendingWork true.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    TicketSyncService svc(deps);
    const TrackerConfig cfg = ConfigFor("fake");

    SUBCASE("the launch skips the task") {
        deps.SkipBackgroundTasks = true;
        svc.SyncWithBackend(&cfg, nullptr);
        CHECK(deps.BackgroundTasksLaunched == 1);
        CHECK_FALSE(svc.HasPendingWork());
    }
    SUBCASE("the launch throws") {
        deps.ThrowOnLaunch = true;
        CHECK_THROWS(svc.SyncWithBackend(&cfg, nullptr));
        CHECK_FALSE(svc.HasPendingWork());
    }
    SUBCASE("a parked task destroyed without running") {
        deps.DeferBackgroundTasks = true;
        svc.SyncWithBackend(&cfg, nullptr);
        CHECK(svc.HasPendingWork());
        deps.DeferredTasks.clear();
        CHECK_FALSE(svc.HasPendingWork());
    }
}

TEST_CASE(
    "TicketSyncService: two async Fulls landing in reverse order do not leave a stray Full for the next request") {
    // The newer Full runs; the older one is dropped. Keeping its Full would turn the unrelated Delta that follows into
    // a second Full.
    DeltaRig rig;
    SeedUnchangedView(rig); // a Delta runs as delta here, a Full as full
    rig.deps.DeferBackgroundTasks = true;
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Full); // older
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Full); // newer
    ReadFilesLeavePostsParked(rig.deps);
    REQUIRE(rig.deps.DeferredPosts.size() == 2);

    rig.deps.RunDeferredPosts(/*reverse=*/true); // the newer Full starts; the older then finds itself overtaken
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == 1);
    CHECK(rig.svc.LastSessionStats().Mode == "full");

    rig.deps.DeferBackgroundTasks = false;
    rig.deps.DeferPosts = false;
    rig.Sync(smatchet::sync::SyncMode::Delta);
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == 2);
    CHECK(rig.svc.LastSessionStats().Mode == "delta"); // not a stray second Full
}

TEST_CASE("TicketSyncService: a Full folded while the teardown gate is up survives a held Delta") {
    // A dropped Full folds while the gate is up, and a Delta held by the gate arrives at EndCacheTeardown: the
    // follow-up keeps the Full instead of being overwritten by the Delta.
    DeltaRig rig;
    SeedUnchangedView(rig);
    rig.deps.DeferBackgroundTasks = true;
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Full);  // older
    rig.svc.SyncWithBackend(&rig.cfg, nullptr, smatchet::sync::SyncMode::Delta); // newer
    ReadFilesLeavePostsParked(rig.deps);
    REQUIRE(rig.deps.DeferredPosts.size() == 2);

    (void)rig.svc.BeginCacheTeardown();
    rig.deps.RunDeferredPosts(/*reverse=*/true); // the Delta is held by the gate; the Full is dropped and folded
    rig.svc.EndCacheTeardown(/*foldIntoFollowUp=*/true);

    rig.deps.DeferBackgroundTasks = false;
    rig.deps.DeferPosts = false;
    rig.Sync(smatchet::sync::SyncMode::Delta); // the recreate's follow-up
    REQUIRE(rig.Settle());
    CHECK(rig.svc.SessionEpoch() == 1);
    CHECK(rig.svc.LastSessionStats().Mode == "full");
}

TEST_CASE("TicketSyncService: a requested sync counts as pending until the post that starts it has run") {
    // Between the files read ending and the session starting, IsActive() is false: a caller waiting for the sync to
    // settle must see HasPendingWork() instead of passing through that gap.
    smatchet_tests::TestEnvGuard env;
    FakeTicketSyncDeps deps;
    deps.DeferBackgroundTasks = true;
    TicketSyncService svc(deps);
    const TrackerConfig cfg = ConfigFor("fake");
    svc.SyncWithBackend(&cfg, nullptr);
    CHECK(svc.HasPendingWork());

    ReadFilesLeavePostsParked(deps); // the read is over; its post has not run
    REQUIRE(deps.DeferredPosts.size() == 1);
    CHECK_FALSE(svc.IsActive());
    CHECK(svc.HasPendingWork()); // still pending: no gap for a waiter to slip through

    deps.RunDeferredPosts(); // the session starts, and the pending mark drops with the post
    CHECK(svc.IsActive());
    CHECK(svc.HasPendingWork());
    CHECK(SpinUntil(svc, [&]() { return !svc.HasPendingWork(); }));
    svc.CancelAndJoinActiveStreamingSync();
}
