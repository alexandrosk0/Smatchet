// MainThreadDispatcherDrain.test.cpp — Slice 2 of
// docs/plans/shipped/pillar-1-2-perf-review-system.md.
//
// Locks the new `dispatcher.drain` perf-scope instrumentation contract
// added to `Source_Core/include/MainThreadDispatcher.h`:
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

#include "doctest/doctest.h"

#include <atomic>
#include <cstddef>

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
