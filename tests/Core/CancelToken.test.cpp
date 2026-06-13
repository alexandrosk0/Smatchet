// CancelToken.test.cpp — WS-A of docs/plans/active/ui-freeze-pillar2-blocking.md.
//
// Pure-logic coverage of the cooperative-cancellation primitives in
// Source/Core/include/Ui/CancelToken.h: the CancelToken shared atomic flag and
// the CancellableFutureSet owner helper (signal-all-cancel, non-blocking
// abandon, teardown join). No AppController / no ImGui — just the helper and a
// stub worker future that observes the flag.

#include "Ui/CancelToken.h"

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using smatchet::ui::CancellableFutureSet;
using smatchet::ui::CancelToken;

TEST_CASE("CancelToken: default is not cancelled; Cancel flips it; copies share the flag") {
    CancelToken t;
    CHECK_FALSE(t.IsCancelled());

    CancelToken copy = t; // shares the underlying atomic flag
    CHECK_FALSE(copy.IsCancelled());

    t.Cancel();
    CHECK(t.IsCancelled());
    CHECK(copy.IsCancelled()); // the worker's copy observes the owner's signal
}

TEST_CASE("CancelToken: Cancel is idempotent") {
    CancelToken t;
    t.Cancel();
    t.Cancel();
    CHECK(t.IsCancelled());
}

TEST_CASE("CancelToken: Reset hands out a fresh flag; old copies keep the old (cancelled) flag") {
    CancelToken t;
    CancelToken oldWorker = t; // captured by a worker that is now being abandoned
    t.Cancel();
    CHECK(oldWorker.IsCancelled());

    t.Reset();
    CHECK_FALSE(t.IsCancelled()); // the owner's handle now points at a fresh flag

    CancelToken newWorker = t;
    CHECK_FALSE(newWorker.IsCancelled()); // a relaunched worker runs against the new flag
    CHECK(oldWorker.IsCancelled());       // the abandoned worker still observes "stop"
}

TEST_CASE("CancellableFutureSet: Add skips invalid futures; Size/Empty track membership") {
    CancellableFutureSet<int> set;
    CHECK(set.Empty());

    std::future<int> invalid; // default-constructed -> not valid
    set.Add(std::move(invalid));
    CHECK(set.Empty());

    std::promise<int> p;
    set.Add(p.get_future());
    CHECK_FALSE(set.Empty());
    CHECK(set.Size() == 1);
}

TEST_CASE("CancellableFutureSet: a worker observing Token() stops promptly after SignalCancelAll") {
    CancellableFutureSet<int> set;
    const CancelToken worker = set.Token();

    std::atomic<bool> started{false};
    auto fut = std::async(std::launch::async, [worker, &started]() {
        started.store(true);
        // Simulate an IO-chunk loop that polls the cancel flag between chunks.
        for (int i = 0; i < 100000; ++i) {
            if (worker.IsCancelled()) {
                return -1; // cooperative early-out
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0; // ran to completion (should not happen in this test)
    });
    set.Add(std::move(fut));

    while (!started.load()) {
        std::this_thread::yield();
    }
    set.SignalCancelAll();

    // JoinAll must return quickly because the worker bailed cooperatively.
    const auto t0 = std::chrono::steady_clock::now();
    set.JoinAll();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(set.Empty());
    CHECK(elapsedMs < 2000); // far below the worker's ~100s uncancelled runtime
}

TEST_CASE("CancellableFutureSet: AbandonInto moves futures out without waiting and resets the token") {
    CancellableFutureSet<int> set;
    const CancelToken before = set.Token();

    std::promise<int> p1;
    std::promise<int> p2;
    set.Add(p1.get_future());
    set.Add(p2.get_future());
    CHECK(set.Size() == 2);

    std::vector<std::future<int>> graveyard;
    set.SignalCancelAll();
    set.AbandonInto(graveyard);

    CHECK(set.Empty());           // futures moved out
    CHECK(graveyard.size() == 2); // ...into the graveyard
    CHECK(before.IsCancelled());  // the abandoned workers still see "stop"

    // The set got a fresh token for the next run.
    CHECK_FALSE(set.Token().IsCancelled());

    // Fulfil the promises so the abandoned futures are well-formed, then drain.
    p1.set_value(1);
    p2.set_value(2);
    for (auto& f : graveyard) {
        CHECK(f.valid());
        (void)f.get();
    }
}

TEST_CASE("CancellableFutureSet: JoinAll swallows a worker exception (shutdown drain never throws)") {
    CancellableFutureSet<int> set;
    auto fut = std::async(std::launch::async, []() -> int { throw std::runtime_error("worker blew up"); });
    set.Add(std::move(fut));
    CHECK_NOTHROW(set.JoinAll());
    CHECK(set.Empty());
}
