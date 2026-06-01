// MainThreadDispatcherDrain.test.cpp — Slice 2 of
// docs/plans/shipped/pillar-1-2-perf-review-system.md.
//
// Locks the new `dispatcher.drain` perf-scope instrumentation contract
// added to `Source/Core/include/MainThreadDispatcher.h`:
//
//   - LastDrainTaskCount() is 0 before any Drain() call.
//   - After Drain(), LastDrainTaskCount() == number of tasks that ran on
//     that drain (the snapshot, not a running counter — it overwrites
//     each call so perf.snapshot sees only the most recent value).
//   - Tasks posted after BeginShutdown() are no-ops and don't grow the
//     count.
//   - The 4096-task bound silently drops the oldest pending task on
//     overflow — Drain() still sees ≤ 4096.
//
// Pure header / no I/O / no UI thread — runs in the bucket-A doctest rig.

#include "MainThreadDispatcher.h"
#include "MainThreadDispatcherDrain.h"

#include "doctest/doctest.h"

#include <atomic>
#include <cstddef>
#include <vector>

TEST_CASE("MainThreadDispatcher::LastDrainTaskCount starts at zero") {
    MainThreadDispatcher d;
    CHECK(d.LastDrainTaskCount() == static_cast<std::size_t>(0));
}

TEST_CASE("MainThreadDispatcher::LastDrainTaskCount reflects most recent drain") {
    MainThreadDispatcher d;
    std::atomic<int> ran{0};

    d.PostToMainThread([&]() { ran.fetch_add(1); });
    d.PostToMainThread([&]() { ran.fetch_add(1); });
    d.PostToMainThread([&]() { ran.fetch_add(1); });

    d.Drain();
    CHECK(ran.load() == 3);
    CHECK(d.LastDrainTaskCount() == static_cast<std::size_t>(3));

    // Second drain with zero posts should snapshot 0 — it's a per-drain
    // count, not a cumulative counter. perf.snapshot consumers depend on
    // this; a cumulative counter would let a quiet frame inherit yesterday's
    // load.
    d.Drain();
    CHECK(ran.load() == 3);
    CHECK(d.LastDrainTaskCount() == static_cast<std::size_t>(0));

    // Third drain with one fresh post.
    d.PostToMainThread([&]() { ran.fetch_add(10); });
    d.Drain();
    CHECK(ran.load() == 13);
    CHECK(d.LastDrainTaskCount() == static_cast<std::size_t>(1));
}

TEST_CASE("MainThreadDispatcher::PostToMainThread no-ops after BeginShutdown") {
    MainThreadDispatcher d;
    std::atomic<int> ran{0};

    d.PostToMainThread([&]() { ran.fetch_add(1); });
    d.BeginShutdown();
    d.PostToMainThread([&]() { ran.fetch_add(100); });
    d.PostToMainThread([&]() { ran.fetch_add(100); });

    d.Drain();
    // Only the pre-shutdown task ran. Post-shutdown posts must be silently
    // dropped — the contract for late-arriving worker posts during AppController
    // teardown.
    CHECK(ran.load() == 1);
    CHECK(d.LastDrainTaskCount() == static_cast<std::size_t>(1));
}

TEST_CASE("MainThreadDispatcher::PostToMainThread respects kMaxQueueSize bound") {
    MainThreadDispatcher d;
    std::atomic<int> ran{0};

    // Pre-C++17 static-constexpr members need an out-of-class definition for
    // ODR-use. Copy to a local to avoid that — the test is verifying behavior,
    // not the symbol address.
    const std::size_t cap = MainThreadDispatcher::kMaxQueueSize;

    // Post one over the cap. The oldest task is dropped, so we see exactly
    // kMaxQueueSize tasks run, not kMaxQueueSize + 1.
    for (std::size_t i = 0; i < cap + 1; ++i) {
        d.PostToMainThread([&]() { ran.fetch_add(1); });
    }
    d.Drain();
    CHECK(ran.load() == static_cast<int>(cap));
    CHECK(d.LastDrainTaskCount() == cap);
}

// ---- Phase 4 drain-time budget: pure requeue step (memory-budget-and-lifetime-hardening.md) ----
// The time-budget path itself is wall-clock dependent (a normal frame drains fully, so a unit
// test cannot deterministically trip it), but the ordering+trim it relies on is pure and tested
// here. RequeueDeferredFront puts the deferred tail ahead of tasks posted during the drain, then
// trims oldest-first to the cap.

TEST_CASE("RequeueDeferredFront: deferred tail leads, arrived follow") {
    std::vector<int> deferred = {1, 2, 3};
    std::vector<int> arrived = {4, 5};
    std::vector<int> out;
    smatchet::RequeueDeferredFront(deferred, arrived, 100, out);
    REQUIRE(out.size() == 5);
    CHECK(out[0] == 1);
    CHECK(out[1] == 2);
    CHECK(out[2] == 3);
    CHECK(out[3] == 4);
    CHECK(out[4] == 5);
}

TEST_CASE("RequeueDeferredFront: trims oldest-first to maxSize") {
    std::vector<int> deferred = {1, 2, 3};
    std::vector<int> arrived = {4, 5, 6};
    std::vector<int> out;
    smatchet::RequeueDeferredFront(deferred, arrived, 4, out);
    // 6 combined, cap 4 -> drop the 2 oldest (1, 2), keep the newest 4 in order.
    REQUIRE(out.size() == 4);
    CHECK(out[0] == 3);
    CHECK(out[1] == 4);
    CHECK(out[2] == 5);
    CHECK(out[3] == 6);
}

TEST_CASE("RequeueDeferredFront: empty inputs and exact-fit are no-op-safe") {
    std::vector<int> empty1, empty2, out;
    smatchet::RequeueDeferredFront(empty1, empty2, 10, out);
    CHECK(out.empty());

    std::vector<int> deferred = {1, 2};
    std::vector<int> arrived = {3, 4};
    smatchet::RequeueDeferredFront(deferred, arrived, 4, out); // exactly at cap -> no trim
    REQUIRE(out.size() == 4);
    CHECK(out[0] == 1);
    CHECK(out[3] == 4);
}
