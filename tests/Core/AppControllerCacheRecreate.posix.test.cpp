// AppController::StartRecreateLocalCacheDatabase: the refusals (no database path, one already running) and the real
// worker path - withdraw the cache on the UI thread, join the pane sync workers, wipe and reopen the file, finish on
// the UI thread - run against a file database in a temp directory (OpenLocalCacheFileForTest) with the test thread
// standing in for the frame loop (it drains the dispatcher the way a frame does). Linked into SmatchetCommandsTests
// with the real (headless) AppController. The cache.recreate command's own handling (waiting caller, no-frame-loop
// pump) is covered by the facet fake in tests/Commands/BuiltinFacetCommands.test.cpp.

#include <doctest/doctest.h>

#include "../support/FakeTicketSyncDeps.h"
#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "GridLiveContext.h"
#include "GridContextDepsAdapter.h"
#include "Persistence/ISyncCache.h"
#include "Sync/TicketSyncService.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

/// A headless controller is never Initialize()d, so it has no tracker backend factory; the follow-up sync a recreate
/// starts would have none to build its backend with. Hand it the fake the TicketSyncService tests use.
void UseFakeBackends(AppController& app) {
    app.SetBackendFactory(std::unique_ptr<ITrackerBackendFactory>(new smatchet_tests::FakeTrackerBackendFactory()));
}

/// Drains the dispatcher until `done` is true (the worker needs the UI thread to withdraw the cache and to finish).
template <typename Pred> bool PumpUntil(AppController& app, Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        app.mainThreadDispatcher.Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// A temp-directory database file the test removes on exit (SQLite side files included).
struct TempDb {
    std::string Path;
    TempDb() { Path = ConfigManager::GetUserDataDirectory() + "recreate_test.sqlite"; }
    ~TempDb() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((Path + suffix).c_str());
        }
    }
};

} // namespace

TEST_CASE("AppController::StartRecreateLocalCacheDatabase refuses without a database path and keeps the gate down") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app; // never Initialize()d: no local cache database path is configured
    CHECK_FALSE(app.IsLocalCacheRecreating());

    bool called = false;
    const VoidResult started =
        app.StartRecreateLocalCacheDatabase([&called](const smatchet::sync::CacheRecreateOutcome&) { called = true; });
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == "Local cache database path is not set.");
    CHECK_FALSE(app.IsLocalCacheRecreating()); // the refusal lowered the gate again
    CHECK_FALSE(called);                       // a refusal never calls back

    // A second refusal says the same: nothing latched.
    const VoidResult again = app.StartRecreateLocalCacheDatabase(nullptr);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == "Local cache database path is not set.");
}

TEST_CASE("AppController cache recreate: withdraws, wipes the queued rows, reopens, and finishes on the UI thread") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    AppController app;
    GridContextDepsAdapter adapter(app); // the deps seam that hands out the live cache
    REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
    UseFakeBackends(app);
    {
        const std::shared_ptr<ISyncCache> cache = adapter.CacheShared();
        REQUIRE(cache);
        cache->EnqueuePendingCreate("Jira", "{\"summary\":\"queued before the wipe\"}");
        REQUIRE(cache->LoadPendingCreates().size() == 1u);
    }

    int doneCalls = 0;
    smatchet::sync::CacheRecreateOutcome outcome;
    const VoidResult started =
        app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome& finished) {
            ++doneCalls;
            outcome = finished;
        });
    REQUIRE(started.has_value());
    CHECK(app.IsLocalCacheRecreating()); // the gate is up from the start

    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));
    CHECK(doneCalls == 1);
    CHECK(outcome.Ok);
    CHECK(outcome.Wiped);
    CHECK_FALSE(app.IsLocalCacheRecreating()); // down again by the time onDone runs
    const std::shared_ptr<ISyncCache> fresh = adapter.CacheShared();
    REQUIRE(fresh);
    CHECK(fresh->LoadPendingCreates().empty()); // queued offline rows go with the file, by design

    // The gate is reusable: a second recreate runs and finishes the same way.
    doneCalls = 0;
    REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                .has_value());
    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));
    CHECK(doneCalls == 1);
}

TEST_CASE("AppController cache recreate: every live pane is emptied and re-armed to sync, not only the focused one") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    AppController app;
    REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
    UseFakeBackends(app);
    GridLiveContext* const other = app.EnsurePaneContextLive("second-pane", "Jira");
    REQUIRE(other != nullptr);
    {
        // The pane shows a row and has already made its first sync.
        std::lock_guard<std::mutex> lock(other->activeTicketsMutex_);
        CachedTicket stale;
        stale.id = "OLD-1";
        other->ActiveTickets.push_back(stale);
        other->activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(other->ActiveTickets);
    }
    other->initialSyncKicked = true;
    other->lastSyncedJql = "project = OLD";

    int doneCalls = 0;
    REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                .has_value());
    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));

    // The row came from the file that is gone: the pane is re-read from the new (empty) database on a worker.
    const auto paneEmpty = [&]() {
        std::lock_guard<std::mutex> lock(other->activeTicketsMutex_);
        return other->ActiveTickets.empty();
    };
    CHECK(PumpUntil(app, paneEmpty));
    // Its first-sync latch is re-armed, so the frame loop's EnsurePaneLiveSyncStarted syncs it again.
    CHECK_FALSE(other->initialSyncKicked);
    CHECK(other->lastSyncedJql.empty());
}

TEST_CASE("AppController cache recreate: a second recreate while one runs is refused and changes nothing") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    AppController app;
    REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
    UseFakeBackends(app);

    int doneCalls = 0;
    REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                .has_value());
    bool secondCalled = false;
    const VoidResult second = app.StartRecreateLocalCacheDatabase(
        [&secondCalled](const smatchet::sync::CacheRecreateOutcome&) { secondCalled = true; });
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().find("already being recreated") != std::string::npos);
    CHECK(app.IsLocalCacheRecreating()); // the refusal must not lower the running recreate's gate

    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));
    CHECK(doneCalls == 1);
    CHECK_FALSE(secondCalled);
}

TEST_CASE("AppController cache recreate: a pane opened mid-recreate starts blocked and is lifted when it finishes") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    AppController app;
    REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
    UseFakeBackends(app);

    int doneCalls = 0;
    REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                .has_value());
    GridLiveContext* const pane = app.EnsurePaneContextLive("pane-opened-mid-recreate", "Jira");
    REQUIRE(pane != nullptr);
    REQUIRE(pane->ticketSync_ != nullptr);
    CHECK(pane->ticketSync_->IsCacheTeardownActive()); // no sync may start against the cache being replaced

    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));
    CHECK_FALSE(pane->ticketSync_->IsCacheTeardownActive()); // lifted with every other service
}

TEST_CASE("AppController cache recreate: shutting down mid-recreate stops the worker and never calls back") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    int doneCalls = 0;
    {
        AppController app;
        REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
        UseFakeBackends(app);
        REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                    .has_value());
        // No pump: the worker is parked waiting for the UI thread to hand the cache over. Destroying the controller
        // must abort that wait and join the worker (a hang here fails the test by timeout).
    }
    CHECK(doneCalls == 0);
}

TEST_CASE(
    "AppController: the one-shot CLI marks itself frame-loop-less, and DrainMainThreadOnce runs the posted tasks") {
    // cache.recreate checks HasFrameLoop() to decide between "started" (a frame loop will finish it) and pumping the
    // queue itself (Source/Standalone/CliCommandRunner.cpp lowers it around the in-process dispatch).
    AppController app;
    CHECK(app.HasFrameLoop()); // the default: a running application draws frames
    app.SetNoFrameLoop(true);
    CHECK_FALSE(app.HasFrameLoop());

    int ran = 0;
    app.PostToMainThread([&ran]() { ++ran; });
    CHECK(ran == 0); // posted, not run: nothing drains it
    app.DrainMainThreadOnce();
    CHECK(ran == 1);

    app.SetNoFrameLoop(false);
    CHECK(app.HasFrameLoop());
}

namespace {
/// Pumps the dispatcher and ticks the service for `ms`, so every sync the UI thread would start or finish has.
void PumpFor(AppController& app, TicketSyncService& sync, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        app.mainThreadDispatcher.Drain();
        sync.TickStreamingApply();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
} // namespace

TEST_CASE("AppController cache recreate: the focused pane's held request rides on the one follow-up sync") {
    // FinishCacheRecreate_ itself starts the follow-up Full sync of the focused pane on success, and folds the pane's
    // held request into it: one session runs. (Replaying the held request first made the follow-up cancel and restart
    // it: two sessions.)
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db;
    AppController app;
    REQUIRE(app.OpenLocalCacheFileForTest(db.Path));
    UseFakeBackends(app);
    GridLiveContext* const pane = app.EnsurePaneContextLive("focused-pane", "Jira");
    REQUIRE(pane != nullptr);
    app.SetFocusedPane("focused-pane");
    TicketSyncService& sync = *pane->ticketSync_;
    const std::uint64_t epochBefore = sync.SessionEpoch();

    int doneCalls = 0;
    REQUIRE(app.StartRecreateLocalCacheDatabase([&](const smatchet::sync::CacheRecreateOutcome&) { ++doneCalls; })
                .has_value());
    sync.SyncWithBackend(nullptr, nullptr, smatchet::sync::SyncMode::Delta); // held by the gate
    REQUIRE(PumpUntil(app, [&]() { return doneCalls > 0; }));
    PumpFor(app, sync, 400); // the follow-up's views load lands, its session starts, any second one would too

    CHECK(sync.SessionEpoch() == epochBefore + 1); // exactly one session after the recreate
}
