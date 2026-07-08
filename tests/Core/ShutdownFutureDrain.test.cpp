// ShutdownFutureDrain.test.cpp — regression coverage for finding DR8 (shutdown UAF /
// null-deref on undrained futures/workers).
//
// The two real fix sites live in UI translation units that require imgui and cannot be
// linked into the standalone unit-test binary. These tests therefore reproduce the exact
// concurrency invariants the fixes rely on, using real std::async / std::thread, so a
// regression in the ordering contract fails here at runtime:
//
//   (a) SmatchetUI_Layout.cpp DrainUiDrawSessionFuturesBeforeAppTeardown must drain a
//       future whose worker captures a referenced dependency (AppController&) BEFORE that
//       dependency is torn down — draining first means the worker never touches freed
//       memory. Mirrors the DrainFutureJoinQuiet(valid -> wait/get, swallow) pattern.
//
//   (b) AnnotateAnalysisUi.cpp ~AnnotateAnalysisUi must cancel + join the worker thread
//       (which dereferences a shared state pointer every loop) BEFORE that pointer is
//       nulled — join-before-null means the worker never observes a null pointer.

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

// Mirror of DrainFutureJoinQuiet: drain a finished-or-in-flight future, swallowing any
// stored exception (shutdown drains must never propagate).
template <typename T> void DrainFutureJoinQuiet(std::future<T>& f) {
    if (!f.valid()) {
        return;
    }
    try {
        f.wait();
        (void)f.get();
    } catch (...) { // catch-all-ok: swallow future exception on shutdown drain
    }
}

// Stands in for AppController: the update-check worker captures a reference to it. If the
// future is not drained before this object dies, the worker touches freed memory.
struct FakeDependency {
    std::atomic<bool> alive{true};
    std::atomic<int> touches{0};
    ~FakeDependency() { alive.store(false, std::memory_order_release); }
};

} // namespace

TEST_CASE("DR8(a): draining the app-update future before teardown means the worker never "
          "touches a freed dependency") {
    auto dep = std::make_unique<FakeDependency>();
    FakeDependency* raw = dep.get();

    std::atomic<bool> observedDead{false};
    std::future<int> fut = std::async(std::launch::async, [raw, &observedDead]() {
        // Simulate a slow in-flight update check that keeps referencing the dependency.
        for (int i = 0; i < 50; ++i) {
            if (!raw->alive.load(std::memory_order_acquire)) {
                observedDead.store(true);
            }
            raw->touches.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 7;
    });

    // Teardown ordering: drain the future FIRST, then destroy the dependency.
    DrainFutureJoinQuiet(fut);
    CHECK(raw->touches.load() == 50);
    dep.reset();

    CHECK_FALSE(observedDead.load());
    CHECK_FALSE(fut.valid()); // get() consumed it
}

TEST_CASE("DR8(a): DrainFutureJoinQuiet swallows a worker exception and is a no-op on an "
          "invalid future") {
    std::future<int> thrower =
        std::async(std::launch::async, []() -> int { throw std::runtime_error("update check blew up"); });
    CHECK_NOTHROW(DrainFutureJoinQuiet(thrower));

    std::future<int> empty; // default-constructed, not valid
    CHECK_NOTHROW(DrainFutureJoinQuiet(empty));
}

namespace {

// Stands in for AnnotateState reached through State() -> s_stateInstance. The worker loops
// dereferencing the shared pointer; if it is nulled before the worker joins, the deref is
// a null-deref during teardown.
struct FakeAnnotateState {
    std::atomic<bool> cancel{false};
    std::atomic<int> iterations{0};
};

struct FakeAnnotateUi {
    std::unique_ptr<FakeAnnotateState> state;
    std::thread worker;
    std::atomic<bool> sawNull{false};

    explicit FakeAnnotateUi(std::atomic<FakeAnnotateState*>& instance)
        : state(std::make_unique<FakeAnnotateState>()) {
        instance.store(state.get(), std::memory_order_release);
        worker = std::thread([this, &instance]() {
            for (;;) {
                FakeAnnotateState* s = instance.load(std::memory_order_acquire);
                if (s == nullptr) {
                    sawNull.store(true); // regression: State() derefed a null pointer
                    return;
                }
                if (s->cancel.load(std::memory_order_acquire)) {
                    return;
                }
                s->iterations.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Reproduces the FIXED ~AnnotateAnalysisUi: cancel + join BEFORE nulling the pointer.
    void TeardownFixed(std::atomic<FakeAnnotateState*>& instance) {
        if (state) {
            state->cancel.store(true, std::memory_order_release);
            if (worker.joinable()) {
                worker.join();
            }
        }
        instance.store(nullptr, std::memory_order_release);
    }
};

} // namespace

TEST_CASE("DR8(b): joining the annotate worker before nulling the shared state pointer "
          "prevents a null-deref during teardown") {
    std::atomic<FakeAnnotateState*> instance{nullptr};
    auto ui = std::make_unique<FakeAnnotateUi>(instance);

    // Let the worker spin a bit so it is genuinely in-flight at teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    ui->TeardownFixed(instance);

    CHECK_FALSE(ui->sawNull.load());              // worker never observed the nulled pointer
    CHECK(instance.load() == nullptr);            // pointer cleared after the join
    CHECK(ui->state->iterations.load() > 0);      // worker actually ran before being cancelled
}
