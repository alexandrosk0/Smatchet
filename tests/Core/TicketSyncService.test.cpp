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
#include "Sync/DeltaSyncPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
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
