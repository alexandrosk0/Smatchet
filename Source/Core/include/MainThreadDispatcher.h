#pragma once

#include "MainThreadDispatcherDrain.h"
#include "UiPerfMonitor.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

/// Bounded, thread-safe queue of tasks drained once per frame on the main (UI) thread.
/// Workers post lambdas instead of setting ad-hoc one-shot atomic/bool flags, centralising
/// the UI-thread-callback contract (BACKLOG_CODE_REVIEW.md §6.1). Drain is called at the head of
/// SmatchetUI::Draw so tasks execute before any window drawing begins that frame.
/// Lifetime contract: the dispatcher is typically a member of `AppController`. Callers MUST
/// stop posting (via `BeginShutdown()`) and join all worker threads before `~AppController`
/// runs — `BeginShutdown()` flips a shutdown atom so late posts no-op rather than touching
/// the about-to-be-destroyed mutex.
/// Bound: `kMaxQueueSize` tasks. On overflow the oldest task is dropped silently. This bound
/// existed in the original docstring but was not enforced; this implementation enforces it.
class MainThreadDispatcher {
  public:
    using Task = std::function<void()>;

    /// Maximum number of pending tasks. Posts beyond this drop the oldest pending task on the
    /// floor so a runaway producer cannot grow memory without bound. 4096 mirrors Logger's
    /// file-sink bound and is well above any reasonable per-frame burst.
    static constexpr std::size_t kMaxQueueSize = 4096;

    /// Per-frame drain-time budget (Phase 4). The count cap above bounds the queue's *size*; this
    /// bounds the *work per frame*. `Drain()` runs tasks FIFO until this budget is exceeded, then
    /// defers the untouched tail to the next frame (Risk #134: the old drain ran the whole queue
    /// unbudgeted, so a burst of decode→upload completions could spike a single frame). Generous
    /// on purpose: a normal frame drains far under it and behaves exactly as before — only a
    /// genuine spike spreads across frames. Ignored during shutdown so the final drain runs all.
    /// Exposed as a constexpr function (not a static constexpr member): a class-type constexpr
    /// member is ODR-used when bound by reference (here `time_point + duration`), which in C++14
    /// requires an out-of-line definition; MSVC and optimised Clang fold the constant and never
    /// emit the reference, but a `-O0` Android debug build does — so the member form fails to link
    /// libSmatchetMobile.so. A constexpr function is implicitly inline and needs no definition.
    static constexpr std::chrono::microseconds DrainBudget() { return std::chrono::microseconds{4000}; }

    /// Post a task to be run on the UI thread at the next `Drain()`. Safe to call from any
    /// thread. No-ops if `BeginShutdown()` has been called.
    void PostToMainThread(Task t) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (shuttingDown_.load(std::memory_order_relaxed)) {
            // Re-check under the lock so a teardown that started between the unlocked check
            // and the lock acquisition does not see a partially-posted task.
            return;
        }
        if (queue_.size() >= kMaxQueueSize) {
            // Drop oldest: vector erase from front is O(N) but only fires at the cap, which is
            // pathological. If the dispatcher saturates regularly switch to std::deque.
            queue_.erase(queue_.begin());
        }
        queue_.push_back(std::move(t));
    }

    /// Drain queued tasks on the calling thread (must be the UI thread), FIFO, until the queue
    /// empties or `DrainBudget()` is exceeded — in which case the untouched tail is requeued ahead
    /// of any task posted meanwhile and runs next frame. Tasks are moved out and each `Task` is
    /// released after invocation so captures (especially shared_ptr / large state) do not live
    /// across the whole drain loop.
    /// Pillar 1 + 2 perf-review (slice 2 of `docs/plans/shipped/pillar-1-2-perf-review-system.md`):
    /// the drain itself is wrapped in `SMATCHET_UI_PERF_SCOPE("dispatcher.drain")` so an
    /// unbounded posted lambda surfaces in `perf.snapshot` as a single hot row. The
    /// `lastDrainTaskCount_` accessor below lets perf-snapshot expose how many tasks ran
    /// without forcing every caller to instrument its own post-back lambdas.
    void Drain() {
        SMATCHET_UI_PERF_SCOPE("dispatcher.drain");
        std::vector<Task> tasks;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tasks.swap(queue_);
        }
        const bool budgeted = !shuttingDown_.load(std::memory_order_acquire);
        const auto deadline = std::chrono::steady_clock::now() + DrainBudget();
        std::size_t ran = 0;
        std::size_t deferred = 0;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i]) {
                tasks[i]();
                tasks[i] = nullptr; // release captures eagerly; previous code held them across the loop
                ++ran;
            }
            // Check the clock only after running at least one task (always make forward progress)
            // and only when work remains. During shutdown the budget is ignored so the final drain
            // empties the queue rather than orphaning the tail.
            if (budgeted && i + 1 < tasks.size() && std::chrono::steady_clock::now() >= deadline) {
                std::vector<Task> tail;
                tail.reserve(tasks.size() - (i + 1));
                for (std::size_t j = i + 1; j < tasks.size(); ++j) {
                    tail.push_back(std::move(tasks[j]));
                }
                deferred = tail.size();
                std::lock_guard<std::mutex> lk(mutex_);
                std::vector<Task> merged;
                smatchet::RequeueDeferredFront(tail, queue_, kMaxQueueSize, merged);
                queue_.swap(merged);
                break;
            }
        }
        lastDrainTaskCount_.store(ran, std::memory_order_release);
        lastDrainDeferredCount_.store(deferred, std::memory_order_release);
    }

    /// Stop accepting new posts. After this returns, all `PostToMainThread` calls become
    /// no-ops, even if they're already past the early atomic check (re-checked under lock).
    /// Drain remaining tasks one last time on the UI thread before destruction.
    void BeginShutdown() { shuttingDown_.store(true, std::memory_order_release); }

    /// Number of tasks drained on the most recent `Drain()` call. Read by
    /// `perf.snapshot` so the per-frame dispatcher load is visible without
    /// requiring every poster to instrument its own SMATCHET_UI_PERF_SCOPE.
    /// Returns 0 before the first drain.
    std::size_t LastDrainTaskCount() const noexcept { return lastDrainTaskCount_.load(std::memory_order_acquire); }

    /// Tasks deferred to the next frame on the most recent `Drain()` because the drain-time
    /// budget was hit (0 in the common case where the queue drained fully). Surfaced by the
    /// `perf.memory` gauge so a sustained non-zero value flags a producer outrunning the budget.
    std::size_t LastDrainDeferredCount() const noexcept {
        return lastDrainDeferredCount_.load(std::memory_order_acquire);
    }

    /// Pending-task count right now, for the `perf.memory` gauge. Snapshot-only —
    /// takes `mutex_` (which `mutable` permits in this const accessor). Not for
    /// flow-control: a non-zero value can drain to 0 the next frame.
    std::size_t QueueLen() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Task> queue_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<std::size_t> lastDrainTaskCount_{0};
    std::atomic<std::size_t> lastDrainDeferredCount_{0};
};
