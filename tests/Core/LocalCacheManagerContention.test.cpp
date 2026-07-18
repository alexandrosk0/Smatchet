// LocalCacheManager concurrent-open contention — Slice G Phase 3 / G2
// (docs/plans/shipped/slice-g-db-corruption.md § Phase 3).
//
// Phase 2 hardened the ctor against a CORRUPT file; it deliberately RE-THREW a transient
// SQLITE_BUSY (never quarantine a healthy-but-locked cache). G2 closes the remaining gap: when a
// SECOND Smatchet process/connection opens the SAME on-disk cache while the first is initialising
// schema, the write-lock conflict must NOT crash the ctor (Quality Pillar 3 "never crash"). The
// fix is bounded backoff-and-retry around InitSchema (keyed on IsTransientBusyCode) layered on the
// busy_timeout that already makes SQLite wait on lock acquisition. The retry POLICY is pinned
// purely in SqliteOpenRecoveryPure.test.cpp; this suite pins the end-to-end contract that a
// stampede of concurrent opens on one path all construct without throwing.
//
// File-backed by necessity: ":memory:" is per-connection, so two connections cannot contend on
// the same bytes. [high-risk] — timing-dependent by nature (it provokes real lock contention), so
// it is quarantined out of the deterministic authoritative run and exercised on the flaky lane.

#include "../support/TempDbFile.h"

#include "LocalCacheManager.h" // pulls CachedTicketTypes.h (CachedTicket) transitively

#include <doctest/doctest.h>

#include <atomic>
#include <exception>
#include <string>
#include <thread>
#include <vector>

using smatchet_tests::TempDbFile;

namespace {
constexpr const char* kBk = "Jira";
} // namespace

TEST_CASE("LocalCacheManager: a stampede of concurrent opens on one path all construct without "
          "throwing (Pillar-3 never-crash under contention)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_contention_");

    constexpr int kThreads = 8;
    // Each thread persists SEVERAL tickets so the connections race the WRITE lock repeatedly, not
    // just the ctor's CREATE TABLE — this exercises the G2 write-path retry (SaveTicket's bounded
    // backoff-and-retry), the write half of "never-crash under contention". Without it an immediate
    // WAL BUSY on a concurrent commit (which busy_timeout does not absorb) escaped SaveTicket as an
    // uncaught throw.
    constexpr int kWritesPerThread = 3;
    std::atomic<bool> start{false};
    std::atomic<int> succeeded{0};
    std::vector<std::string> failures(kThreads);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i]() {
            // Spin until the gate opens so all threads hit ctor/schema-init at once, maximizing the
            // window where two connections race the CREATE TABLE write lock.
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                LocalCacheManager mgr(tmp.Path());
                // Prove the connection is usable, not merely constructed: a read + repeated
                // namespaced writes all succeed under WAL even while siblings hold the file open.
                (void)mgr.GetAllTicketIds(kBk);
                for (int w = 0; w < kWritesPerThread; ++w) {
                    CachedTicket t;
                    t.id = "T-" + std::to_string(i) + "-" + std::to_string(w);
                    t.fieldValues["summary"] = "row from thread " + std::to_string(i);
                    mgr.SaveTicket(kBk, t); // must absorb a transient write-lock BUSY, never throw
                }
                succeeded.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& ex) {
                failures[i] = ex.what();
            } catch (...) {
                failures[i] = "non-std exception";
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    // Every concurrent open must have survived — a transient BUSY during schema-init is absorbed
    // by busy_timeout + the G2 retry, never propagated as an uncaught ctor throw.
    for (int i = 0; i < kThreads; ++i) {
        INFO("thread ", i, " failure: ", failures[i]);
        CHECK(failures[i].empty());
    }
    CHECK(succeeded.load() == kThreads);

    // The shared cache is coherent afterwards: every thread's namespaced writes all landed —
    // kThreads * kWritesPerThread distinct rows, proving the write-path retry preserved every commit.
    LocalCacheManager reader(tmp.Path());
    CHECK(reader.GetAllTicketIds(kBk).size() == static_cast<std::size_t>(kThreads * kWritesPerThread));
}
