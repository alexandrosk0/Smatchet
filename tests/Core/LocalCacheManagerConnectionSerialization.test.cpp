// LocalCacheManager's one shared SQLite connection is serialized: every public method holds the connection
// guard for its whole operation, so one thread's rollback can never undo another thread's write and no two
// transactions nest on the connection. Also pins the re-entry contract (a same-thread nested call throws) and the
// foreign-guard check, and carries the hidden hold-time benchmark.

#include "LocalCacheManager.h"

#include "SqliteMemFixture.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Friend of LocalCacheManager and its ConnGuard: the one door tests use to hold the connection.
struct LocalCacheManagerTestAccess {
    template <typename Fn> static void WithGuard(LocalCacheManager& mgr, Fn&& fn) {
        const LocalCacheManager::ConnGuard guard(mgr, "TestHold");
        fn(guard);
    }
    static SQLite::Database& Conn(LocalCacheManager& mgr, const LocalCacheManager::ConnGuard& guard) {
        return mgr.conn(guard);
    }
};

namespace {

constexpr const char* kBk = "Jira@test#1";

std::vector<std::int64_t> ActiveCreateIds(LocalCacheManager& mgr) {
    std::vector<std::int64_t> ids;
    for (const PendingCreate& row : mgr.LoadPendingCreates()) {
        ids.push_back(row.Id);
    }
    return ids;
}

} // namespace

TEST_CASE("LocalCacheManager connection: a failing Archive's rollback never undoes a concurrent Enqueue") {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();
    constexpr int kRounds = 200;

    std::atomic<int> unexpected{0};
    std::thread failing([&]() {
        for (int i = 0; i < kRounds; ++i) {
            try {
                mgr.ArchivePendingCreate(1000000 + i, "test", ""); // no such row: throws after BEGIN, so it rolls back
                unexpected.fetch_add(1);
            } catch (const std::runtime_error&) {
                // expected: the row is absent
            }
        }
    });
    std::vector<std::int64_t> enqueued;
    std::thread writer([&]() {
        for (int i = 0; i < kRounds; ++i) {
            enqueued.push_back(mgr.EnqueuePendingCreate(kBk, "{\"summary\":\"row\"}"));
        }
    });
    failing.join();
    writer.join();

    CHECK(unexpected.load() == 0);
    REQUIRE(enqueued.size() == static_cast<std::size_t>(kRounds));
    CHECK(ActiveCreateIds(mgr) == enqueued);
}

TEST_CASE("LocalCacheManager connection: four threads cycling enqueue, archive and restore never nest a BEGIN") {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();
    constexpr int kThreads = 4;
    constexpr int kCycles = 60;

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kCycles; ++i) {
                try {
                    const std::int64_t id = mgr.EnqueuePendingCreate(kBk, "{\"summary\":\"cycle\"}");
                    mgr.ArchivePendingCreate(id, "test", "err");
                    if (!mgr.RestoreDeadPendingCreate(id)) {
                        failures.fetch_add(1);
                    }
                } catch (const std::exception&) {
                    failures.fetch_add(1); // includes "cannot start a transaction within a transaction"
                }
            }
            (void)t;
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }

    CHECK(failures.load() == 0);
    // Every cycle ends with its row restored and no dead row left behind.
    CHECK(mgr.LoadPendingCreates().size() == static_cast<std::size_t>(kThreads * kCycles));
    CHECK(mgr.GetDeadPendingCreateCount() == 0);
}

TEST_CASE("LocalCacheManager connection: a same-thread nested call throws instead of deadlocking") {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();

    LocalCacheManagerTestAccess::WithGuard(mgr, [&](const LocalCacheManager::ConnGuard&) {
        CHECK_THROWS_AS(mgr.HasCacheMetaFlag("anything"), std::logic_error);
        CHECK_THROWS_AS(mgr.EnqueuePendingCreate(kBk, "{}"), std::logic_error);
        CHECK_THROWS_AS(mgr.GetAllTickets(kBk), std::logic_error);
        // The degrade paths swallow the throw and report failure rather than crash.
        CHECK_FALSE(mgr.UpsertLookup(kBk, "kind", "key", "{}"));
        std::vector<smatchet::LeftViewRecord> none;
        CHECK_FALSE(mgr.CommitLeftViewDepartures(none, 10, kBk, "view", "{}", 1));
    });

    // The guard released cleanly: the manager is usable again, and nothing was half-written.
    CHECK_FALSE(mgr.HasCacheMetaFlag("anything"));
    CHECK(mgr.EnqueuePendingCreate(kBk, "{}") > 0);
}

TEST_CASE("LocalCacheManager connection: another manager's guard does not open this connection") {
    smatchet_tests::SqliteMemFixture a;
    smatchet_tests::SqliteMemFixture b;
    LocalCacheManagerTestAccess::WithGuard(a.Ref(), [&](const LocalCacheManager::ConnGuard& guardOfA) {
        CHECK_NOTHROW(LocalCacheManagerTestAccess::Conn(a.Ref(), guardOfA));
        CHECK_THROWS_AS(LocalCacheManagerTestAccess::Conn(b.Ref(), guardOfA), std::logic_error);
        // Two managers have two connections: holding A never blocks or re-enters B.
        CHECK_FALSE(b.Ref().HasCacheMetaFlag("x"));
    });
}

TEST_CASE("LocalCacheManager connection: a second thread waits for the held connection, then proceeds") {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread waiter;
    LocalCacheManagerTestAccess::WithGuard(mgr, [&](const LocalCacheManager::ConnGuard&) {
        waiter = std::thread([&]() {
            started.store(true);
            (void)mgr.HasCacheMetaFlag("x");
            finished.store(true);
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(finished.load()); // still blocked behind the guard
    });
    waiter.join();
    CHECK(finished.load());
}

// Hold-time evidence for the lock model. Hidden: run with `[.bench]` (prints, never asserts a wall-clock budget).
TEST_CASE("LocalCacheManager connection: probe wait during long operations" * doctest::skip(true)) {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();
    std::vector<CachedTicket> seed;
    for (int i = 0; i < 10000; ++i) {
        CachedTicket t;
        t.id = "BENCH-" + std::to_string(i);
        t.fieldValues["summary"] = "summary " + std::to_string(i);
        t.fieldValues["status"] = "Open";
        seed.push_back(std::move(t));
    }
    mgr.SaveTickets(kBk, seed);

    std::atomic<bool> stop{false};
    std::atomic<long long> maxWaitUs{0};
    std::atomic<long long> probes{0};
    std::thread probe([&]() {
        while (!stop.load()) {
            const auto t0 = std::chrono::steady_clock::now();
            (void)mgr.HasCacheMetaFlag("bench");
            const long long us =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            long long cur = maxWaitUs.load();
            while (us > cur && !maxWaitUs.compare_exchange_weak(cur, us)) {
            }
            probes.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    (void)mgr.GetAllTickets(kBk);
    const long long allMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    stop.store(true);
    probe.join();
    MESSAGE("GetAllTickets(10k) took " << allMs << " ms; " << probes.load() << " probes; max probe wait "
                                       << maxWaitUs.load() << " us");
}
