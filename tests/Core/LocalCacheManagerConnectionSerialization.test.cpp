// LocalCacheManager's one shared SQLite connection is serialized: every public method holds the connection
// guard for its whole operation, so one thread's rollback can never undo another thread's write and no two
// transactions nest on the connection. Also pins the re-entry contract (a same-thread nested call throws) and the
// foreign-guard check, and carries the hidden hold-time benchmark.

#include "LocalCacheManager.h"

#include "SqliteMemFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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

TEST_CASE("LocalCacheManager connection: every concurrent enqueue gets the id of its own row") {
    smatchet_tests::SqliteMemFixture fix;
    LocalCacheManager& mgr = fix.Ref();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::vector<std::vector<std::pair<std::int64_t, std::string>>> issued(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string payload = "{\"t\":" + std::to_string(t) + ",\"i\":" + std::to_string(i) + "}";
                issued[t].push_back(std::make_pair(mgr.EnqueuePendingCreate(kBk, payload), payload));
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }

    std::unordered_map<std::int64_t, std::string> stored;
    for (const PendingCreate& row : mgr.LoadPendingCreates()) {
        stored[row.Id] = row.Payload;
    }
    CHECK(stored.size() == static_cast<std::size_t>(kThreads * kPerThread));
    for (int t = 0; t < kThreads; ++t) {
        for (const std::pair<std::int64_t, std::string>& one : issued[t]) {
            const std::unordered_map<std::int64_t, std::string>::const_iterator it = stored.find(one.first);
            REQUIRE(it != stored.end());
            CHECK(it->second == one.second); // the id this thread was handed is the row it wrote
        }
    }
}

namespace {

struct ProbeResult {
    long long OperationMs = 0;
    long long Probes = 0;
    long long MaxWaitUs = 0;
};

// Runs `operation` while another thread probes the connection (a cheap guarded read) and reports the longest wait.
template <typename Fn> ProbeResult MeasureProbeWait(LocalCacheManager& mgr, Fn&& operation) {
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
    operation();
    ProbeResult result;
    result.OperationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    stop.store(true);
    probe.join();
    result.Probes = probes.load();
    result.MaxWaitUs = maxWaitUs.load();
    return result;
}

} // namespace

// Hold-time evidence for the lock model. Hidden: run with `--no-skip` (it prints, never asserts a wall-clock budget).
TEST_CASE("LocalCacheManager connection: probe wait during long operations [.bench]" * doctest::skip(true)) {
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

    const ProbeResult all = MeasureProbeWait(mgr, [&]() { (void)mgr.GetAllTickets(kBk); });
    MESSAGE("GetAllTickets(10k): " << all.OperationMs << " ms, " << all.Probes << " probes, max probe wait "
                                   << all.MaxWaitUs << " us");

    std::vector<CachedTicket> batch(seed.begin(), seed.begin() + 500);
    std::vector<TicketSyncMeta> metas(batch.size());
    for (std::size_t i = 0; i < metas.size(); ++i) {
        metas[i].RemoteUpdated = "2026-01-01T00:00:00.000+0000";
        metas[i].SyncedAtEpochSec = 1;
    }
    const ProbeResult save = MeasureProbeWait(mgr, [&]() { mgr.SaveTicketsWithSyncMeta(kBk, batch, metas, true); });
    MESSAGE("SaveTicketsWithSyncMeta(500): " << save.OperationMs << " ms, " << save.Probes << " probes, max probe wait "
                                             << save.MaxWaitUs << " us");

    const ProbeResult rekey = MeasureProbeWait(mgr, [&]() {
        mgr.RunOneTimeCacheIdentityRekey(std::vector<std::pair<std::string, std::string>>(
            1, std::make_pair(std::string(kBk), std::string("Jira@bench#2"))));
    });
    MESSAGE("RunOneTimeCacheIdentityRekey(10k rows): " << rekey.OperationMs << " ms, " << rekey.Probes
                                                       << " probes, max probe wait " << rekey.MaxWaitUs << " us");

    // An enqueue arriving while a worker batch runs: its latency is the wait for the connection plus its own insert.
    std::atomic<bool> batchDone{false};
    std::thread worker([&]() {
        mgr.SaveTicketsWithSyncMeta("Jira@bench#2", seed, std::vector<TicketSyncMeta>(), true);
        batchDone.store(true);
    });
    long long worstUs = 0;
    int enqueued = 0;
    while (!batchDone.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)mgr.EnqueuePendingCreate(kBk, "{}");
        const long long us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        worstUs = (std::max)(worstUs, us);
        ++enqueued;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    worker.join();
    MESSAGE("EnqueuePendingCreate during a 10k-row SaveTicketsWithSyncMeta batch: " << enqueued << " enqueues, worst "
                                                                                    << worstUs << " us");
}
