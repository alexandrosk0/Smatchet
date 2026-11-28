// The startup cache open (AppController::LaunchStartupCacheOpen_): the open runs on a worker behind the cache gate,
// the controller reports Opening and refuses durable enqueues and a recreate meanwhile, publishes the cache from the UI
// thread when the worker is done, degrades to a memory-only cache when the file will not open, and can retry. Run
// against file databases in a temp directory with the test thread standing in for the frame loop. Linked into
// SmatchetCommandsTests with the real (headless) AppController.

#include <doctest/doctest.h>

#include "../support/FakeTicketSyncDeps.h"
#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "GridContextDepsAdapter.h"
#include "LeftViewHistoryService.h"
#include "LocalCacheManager.h"
#include "Persistence/LocalCacheStatusPure.h"
#include "Tracker/IssueDraft.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <ghc/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

using smatchet::persistence::CacheEnqueueBlock;
using smatchet::persistence::LocalCacheState;

template <typename Pred> bool PumpUntil(AppController& app, Pred done, int seconds = 20) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
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
    explicit TempDb(const char* name) { Path = ConfigManager::GetUserDataDirectory() + name; }
    ~TempDb() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((Path + suffix).c_str());
        }
    }
};

/// Another connection that creates the file in rollback-journal mode and holds its exclusive lock, so an open of
/// the same file has to wait.
struct HeldLock {
    SQLite::Database Holder;
    explicit HeldLock(const std::string& path) : Holder(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
        Holder.exec("CREATE TABLE held (a INTEGER)");
        Holder.exec("BEGIN EXCLUSIVE");
    }
    void Release() { Holder.exec("ROLLBACK"); }
};

constexpr const char* kKey = "Jira@test#1";

} // namespace

TEST_CASE("startup cache open: Opening refuses enqueues and a recreate, then publishes a Ready cache") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db("startup_open_contended.sqlite");
    HeldLock lock(db.Path);
    AppController app;
    app.StartStartupCacheOpenForTest(db.Path, kKey);

    CHECK(app.IsLocalCacheOpening());
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::Opening);
    CHECK(app.GetLocalCacheStatus().State == LocalCacheState::Opening);
    CHECK_FALSE(app.WaitForLocalCacheOpen(std::chrono::milliseconds(50))); // still waiting on the lock
    const VoidResult recreate = app.StartRecreateLocalCacheDatabase(nullptr);
    REQUIRE_FALSE(recreate.has_value());
    CHECK(recreate.error().find("still opening") != std::string::npos);
    CHECK(GridContextDepsAdapter(app).CacheShared() == nullptr);

    lock.Release();
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    CHECK_FALSE(app.IsLocalCacheOpening());
    CHECK(app.GetLocalCacheStatus().State == LocalCacheState::Ready);
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::None);
    CHECK_FALSE(app.IsLocalCacheRecreating()); // the gate is down again
    const std::shared_ptr<ISyncCache> cache = GridContextDepsAdapter(app).CacheShared();
    REQUIRE(cache);
    CHECK(cache->EnqueuePendingCreate(kKey, "{}") > 0);
    CHECK(app.TakeLocalCacheStartupNotice().empty()); // a healthy open has nothing to say
}

TEST_CASE("startup cache open: a file that will not open degrades to a session-only cache, with a notice") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string missingDir = ConfigManager::GetUserDataDirectory() + "no_such_dir_for_cache/";
    AppController app;
    app.StartStartupCacheOpenForTest(missingDir + "cache.sqlite", kKey);

    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    const smatchet::persistence::LocalCacheStatus status = app.GetLocalCacheStatus();
    CHECK(status.State == LocalCacheState::SessionOnly);
    CHECK_FALSE(status.LastError.empty());
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::SessionOnly);
    CHECK(GridContextDepsAdapter(app).CacheShared() != nullptr); // a memory-only cache serves reads for the session
    const std::string notice = app.TakeLocalCacheStartupNotice();
    CHECK_FALSE(notice.empty());
    CHECK(app.TakeLocalCacheStartupNotice().empty()); // one-shot
    const VoidResult recreate = app.StartRecreateLocalCacheDatabase(nullptr);
    REQUIRE_FALSE(recreate.has_value());
    CHECK(recreate.error().find("not open on its database file") != std::string::npos);
}

TEST_CASE("startup cache open: a retry after the cause is fixed reaches Ready and a full sync follows") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string dir = ConfigManager::GetUserDataDirectory() + "retry_cache_dir/";
    TempDb db("retry_cache_dir/cache.sqlite");
    db.Path = dir + "cache.sqlite";
    AppController app;
    app.SetBackendFactory(std::make_unique<smatchet_tests::FakeTrackerBackendFactory>());
    app.StartStartupCacheOpenForTest(db.Path, kKey);
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    REQUIRE(app.GetLocalCacheStatus().State == LocalCacheState::SessionOnly);

    // Fix the cause: the directory now exists.
    ghc::filesystem::create_directories(dir);
    const VoidResult reopened = app.StartReopenLocalCache();
    REQUIRE(reopened.has_value());
    CHECK(app.IsLocalCacheOpening());
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    CHECK(app.GetLocalCacheStatus().State == LocalCacheState::Ready);
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::None);
    const std::shared_ptr<ISyncCache> cache = GridContextDepsAdapter(app).CacheShared();
    REQUIRE(cache);
    CHECK(cache->EnqueuePendingCreate(kKey, "{}") > 0);

    // Open already: nothing to retry.
    const VoidResult again = app.StartReopenLocalCache();
    CHECK_FALSE(again.has_value());
    std::error_code ec;
    ghc::filesystem::remove_all(dir, ec);
}

TEST_CASE("startup cache open: destroying the controller during a contended open ends it promptly") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db("startup_open_destroy.sqlite");
    HeldLock lock(db.Path);
    const auto started = std::chrono::steady_clock::now();
    {
        AppController app;
        app.StartStartupCacheOpenForTest(db.Path, kKey);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the worker is waiting on the lock
        CHECK(app.IsLocalCacheOpening());
    } // ~AppController raises the abort flag and joins the worker
    const long long elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(elapsedMs < 4000); // the busy timeout alone is 5000 ms
    lock.Release();
}

TEST_CASE("startup cache open: an older cache file is copied to the new location and left untouched") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb legacy("startup_legacy_source.sqlite");
    TempDb target("startup_legacy_target.sqlite");
    {
        LocalCacheManager old(legacy.Path);
        IssueDraft draft;
        draft.ProjectKey = "PROJ"; // a draft with a project is left alone by the legacy-project sweep
        REQUIRE(old.EnqueuePendingCreate(kKey, IssueDraftHelpers::ToJson(draft)) > 0);
    }
    const std::int64_t legacyBytes = static_cast<std::int64_t>(ghc::filesystem::file_size(legacy.Path));
    AppController app;
    std::vector<std::string> candidates;
    candidates.push_back(legacy.Path);
    app.StartStartupCacheOpenForTest(target.Path, kKey, candidates);

    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    const smatchet::persistence::LocalCacheStatus status = app.GetLocalCacheStatus();
    CHECK(status.State == LocalCacheState::Ready);
    CHECK_FALSE(status.UsingLegacyPath);
    CHECK(status.ImportedFrom == legacy.Path);
    CHECK(status.Path == target.Path);
    const std::shared_ptr<ISyncCache> cache = GridContextDepsAdapter(app).CacheShared();
    REQUIRE(cache);
    LocalCacheManager* const concrete = dynamic_cast<LocalCacheManager*>(cache.get());
    REQUIRE(concrete != nullptr);
    CHECK(concrete->LoadPendingCreates().size() == 1);
    CHECK(static_cast<std::int64_t>(ghc::filesystem::file_size(legacy.Path)) == legacyBytes);
}

TEST_CASE("startup cache open: another process holding the import lock defers: session-only, nothing created") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb legacy("startup_legacy_deferred_source.sqlite");
    TempDb target("startup_legacy_deferred_target.sqlite");
    {
        LocalCacheManager old(legacy.Path);
        REQUIRE(old.EnqueuePendingCreate(kKey, "{}") > 0);
    }
    // Another process is importing right now: its lock is fresh. Borrowing the file it is copying would strand this
    // session's writes in a file the next start never reads, so this start runs memory-only and creates nothing.
    const std::string lockPath = target.Path + ".init-lock";
    {
        std::ofstream lock(lockPath);
        lock << "held";
    }
    AppController app;
    std::vector<std::string> candidates;
    candidates.push_back(legacy.Path);
    app.StartStartupCacheOpenForTest(target.Path, kKey, candidates);

    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(30)));
    const smatchet::persistence::LocalCacheStatus status = app.GetLocalCacheStatus();
    CHECK(status.State == LocalCacheState::SessionOnly);
    CHECK(status.LastError.find("another Smatchet process") != std::string::npos);
    CHECK_FALSE(ghc::filesystem::exists(target.Path)); // never created outside the lock
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::SessionOnly);
    std::remove(lockPath.c_str());
}

TEST_CASE("startup cache open: a recreate is refused while an older location's file is the open cache") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db("startup_legacy_borrowed.sqlite");
    AppController app;
    app.SetBackendFactory(std::make_unique<smatchet_tests::FakeTrackerBackendFactory>());
    app.StartStartupCacheOpenForTest(db.Path, kKey);
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    REQUIRE(app.GetLocalCacheStatus().State == LocalCacheState::Ready);

    smatchet::persistence::LocalCacheStatus borrowed = app.GetLocalCacheStatus();
    borrowed.UsingLegacyPath = true;
    borrowed.LegacyPath = db.Path;
    app.SetLocalCacheStatusForTest(borrowed);
    const VoidResult recreate = app.StartRecreateLocalCacheDatabase(nullptr);
    REQUIRE_FALSE(recreate.has_value());
    CHECK(recreate.error().find("older location") != std::string::npos);
    CHECK_FALSE(app.IsLocalCacheRecreating()); // the refusal lowered the gate again
}

TEST_CASE("startup cache open: a damaged file is rebuilt, and the user is told once where the damaged copy is") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db("startup_damaged.sqlite");
    {
        std::ofstream out(db.Path, std::ios::binary);
        out << std::string(8192, 'x'); // not a database
    }
    AppController app;
    app.StartStartupCacheOpenForTest(db.Path, kKey);
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));

    const smatchet::persistence::LocalCacheStatus status = app.GetLocalCacheStatus();
    CHECK(status.State == LocalCacheState::Ready);
    REQUIRE(status.LastRecovery.Rebuilt);
    CHECK(status.LastRecovery.FromThisLaunch);
    const std::string quarantined = status.LastRecovery.QuarantinedPath;
    CHECK(ghc::filesystem::exists(quarantined));
    const std::string notice = app.TakeLocalCacheStartupNotice();
    CHECK(notice.find(quarantined) != std::string::npos);
    CHECK(app.TakeLocalCacheStartupNotice().empty()); // once
    CHECK(app.LocalCacheEnqueueBlock() == CacheEnqueueBlock::None);
    std::remove(quarantined.c_str());
}

TEST_CASE("startup cache open: the left-view history saved by an earlier run shows after a normal launch") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TempDb db("startup_left_view.sqlite");
    {
        LocalCacheManager seed(db.Path);
        smatchet::LeftViewRecord row;
        row.backendKey = kKey;
        row.paneId = "pane";
        row.viewId = "view";
        row.viewName = "My view";
        row.issueKey = "ABC-1";
        row.summary = "left the view";
        row.leftAtUnixSec = 1700000000;
        std::vector<smatchet::LeftViewRecord> rows;
        rows.push_back(row);
        REQUIRE(seed.CommitLeftViewDepartures(rows, 100, kKey, "view", "[]", 1700000000));
    }
    AppController app;
    app.StartStartupCacheOpenForTest(db.Path, kKey); // the history's first load runs before the cache is published
    REQUIRE(app.WaitForLocalCacheOpen(std::chrono::seconds(20)));
    REQUIRE(app.LeftViewHistory() != nullptr);
    CHECK(PumpUntil(app, [&]() { return !app.LeftViewHistory()->Rows().empty(); }));
    REQUIRE(app.LeftViewHistory()->Rows().size() == 1);
    CHECK(app.LeftViewHistory()->Rows()[0].issueKey == "ABC-1");
}
