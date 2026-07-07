// MainThreadDispatchShutdown.test.cpp — regression lock for finding DR15.
//
// DR15: RunOnUiThread() (Source/Core/include/Commands/MainThreadDispatch.h) marshals work to
// the UI thread by posting a task that owns a std::promise, then blocking on that promise's
// future. The bounded MainThreadDispatcher can DISCARD a posted task without ever running it:
//   - silently, once BeginShutdown() has flipped its no-op flag (called first in ~AppController,
//     before the background-worker join), and
//   - via the drop-oldest eviction when the 4096-entry queue saturates.
// Before the fix, RunOnUiThread kept its OWN reference to the promise alive across future.get(),
// so a discarded task's destruction left the promise alive-but-unfulfilled: no broken_promise
// fired and future.get() blocked the shutdown join forever.
//
// The fix hands sole promise ownership to the posted task (promise.reset() after posting), so a
// discarded task breaks the future and future.get() unblocks with a shutdown error.
//
// These tests must NEVER hang, even on regression: the blocking RunOnUiThread call runs on a
// DETACHED worker thread whose completion is signalled through a promise/future THIS test owns,
// and every wait is bounded. A regression therefore surfaces as a REQUIRE failure (the bounded
// wait times out), not as a hung test process.
//
// Pure header / no I/O / no real UI thread — runs in the bucket-A doctest rig.

#include "Commands/MainThreadDispatch.h"
#include "Commands/IMainThreadPoster.h"

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Off-UI-thread poster that DROPS every posted task at post time — the exact behaviour of the
// dispatcher's post-BeginShutdown no-op path. The task's std::function is destroyed as this
// method returns, never invoked.
class DroppingPoster : public IMainThreadPoster {
  public:
    bool IsOnUiThread() const override { return false; }
    void PostToMainThread(std::function<void()>) override {}
};

// Off-UI-thread poster that runs the task inline (the ideal drain). Proves the fix leaves the
// happy path — and genuine handler-exception propagation — intact.
class InlinePoster : public IMainThreadPoster {
  public:
    bool IsOnUiThread() const override { return false; }
    void PostToMainThread(std::function<void()> fn) override { fn(); }
};

// Reports being ON the UI thread so RunOnUiThread takes its inline fast path and never posts.
class UiThreadPoster : public IMainThreadPoster {
  public:
    bool IsOnUiThread() const override { return true; }
    void PostToMainThread(std::function<void()>) override { posted_ = true; }
    bool Posted() const { return posted_; }

  private:
    bool posted_ = false;
};

// Off-UI-thread poster that QUEUES tasks and can later evict them without running — models the
// drop-oldest eviction of an already-queued task (the second DR15 discard path).
class QueueingPoster : public IMainThreadPoster {
  public:
    bool IsOnUiThread() const override { return false; }
    void PostToMainThread(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tasks_.push_back(std::move(fn));
        }
        posted_.store(true, std::memory_order_release);
    }
    void EvictAll() {
        std::lock_guard<std::mutex> lk(mutex_);
        tasks_.clear();
    }
    bool Posted() const { return posted_.load(std::memory_order_acquire); }

  private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
    std::atomic<bool> posted_{false};
};

// Drive RunOnUiThread<int> on a detached thread and report, through a caller-owned future, whether
// it finished within `budget` and whether it threw the shutdown std::runtime_error. Detachment is
// what makes a regression a bounded-wait FAILURE rather than a hung process: if RunOnUiThread never
// returns, the detached thread leaks but this helper still returns once the budget elapses.
struct DispatchOutcome {
    bool finished = false;
    bool threwShutdownError = false;
};

DispatchOutcome RunDetachedAndAwait(const std::shared_ptr<IMainThreadPoster>& poster,
                                    std::chrono::milliseconds budget) {
    auto donePromise = std::make_shared<std::promise<void>>();
    auto doneFuture = donePromise->get_future();
    auto threw = std::make_shared<std::atomic<bool>>(false);

    std::thread([poster, donePromise, threw]() {
        try {
            smatchet::cmd::RunOnUiThread<int>(*poster, []() { return 7; });
        } catch (const std::runtime_error&) {
            threw->store(true);
        } catch (...) {
            // Any other escape is a distinct failure; leave threw=false so the CHECK below fails.
        }
        donePromise->set_value();
    }).detach();

    DispatchOutcome outcome;
    outcome.finished = doneFuture.wait_for(budget) == std::future_status::ready;
    outcome.threwShutdownError = threw->load();
    return outcome;
}

constexpr std::chrono::milliseconds kBudget{3000};

} // namespace

TEST_CASE("RunOnUiThread: happy path returns the handler result (inline poster)") {
    InlinePoster poster;
    const int value = smatchet::cmd::RunOnUiThread<int>(poster, []() { return 42; });
    CHECK(value == 42);
}

TEST_CASE("RunOnUiThread: fast path runs inline on the UI thread without posting") {
    UiThreadPoster poster;
    const std::string value =
        smatchet::cmd::RunOnUiThread<std::string>(poster, []() { return std::string("inline"); });
    CHECK(value == "inline");
    CHECK_FALSE(poster.Posted());
}

TEST_CASE("RunOnUiThread: a genuine handler exception propagates unchanged (not masked as shutdown)") {
    InlinePoster poster;
    CHECK_THROWS_AS(smatchet::cmd::RunOnUiThread<int>(
                        poster, []() -> int { throw std::out_of_range("handler boom"); }),
                    std::out_of_range);
}

TEST_CASE("RunOnUiThread: post-shutdown dropped task unblocks with a shutdown error (no hang)") {
    auto poster = std::make_shared<DroppingPoster>();
    const DispatchOutcome outcome = RunDetachedAndAwait(poster, kBudget);
    REQUIRE(outcome.finished); // regression => this times out instead of hanging the process
    CHECK(outcome.threwShutdownError);
}

TEST_CASE("RunOnUiThread: drop-oldest eviction of a queued task unblocks (no hang)") {
    auto poster = std::make_shared<QueueingPoster>();

    auto donePromise = std::make_shared<std::promise<void>>();
    auto doneFuture = donePromise->get_future();
    auto threw = std::make_shared<std::atomic<bool>>(false);

    std::thread([poster, donePromise, threw]() {
        try {
            smatchet::cmd::RunOnUiThread<int>(*poster, []() { return 7; });
        } catch (const std::runtime_error&) {
            threw->store(true);
        } catch (...) {
            // Any other escape leaves threw=false, failing the CHECK below.
        }
        donePromise->set_value();
    }).detach();

    // Wait (bounded) for the task to be queued, then evict it as the drop-oldest cap would.
    const auto deadline = std::chrono::steady_clock::now() + kBudget;
    while (!poster->Posted() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(poster->Posted());
    poster->EvictAll();

    REQUIRE(doneFuture.wait_for(kBudget) == std::future_status::ready);
    CHECK(threw->load());
}
