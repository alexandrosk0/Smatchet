#pragma once

#include "UiPerfMonitor.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

/// Bounded, thread-safe queue of tasks drained once per frame on the main (UI) thread.
///
/// Workers post lambdas instead of setting ad-hoc one-shot atomic/bool flags, centralising
/// the UI-thread-callback contract (BACKLOG_CODE_REVIEW.md §6.1). Drain is called at the head of
/// SmatchetUI::Draw so tasks execute before any window drawing begins that frame.
///
/// Lifetime contract: the dispatcher is typically a member of `AppController`. Callers MUST
/// stop posting (via `BeginShutdown()`) and join all worker threads before `~AppController`
/// runs — `BeginShutdown()` flips a shutdown atom so late posts no-op rather than touching
/// the about-to-be-destroyed mutex.
///
/// Bound: `kMaxQueueSize` tasks. On overflow the oldest task is dropped silently. This bound
/// existed in the original docstring but was not enforced; this implementation enforces it.
class MainThreadDispatcher {
  public:
    using Task = std::function<void()>;

    /// Maximum number of pending tasks. Posts beyond this drop the oldest pending task on the
    /// floor so a runaway producer cannot grow memory without bound. 4096 mirrors Logger's
    /// file-sink bound and is well above any reasonable per-frame burst.
    static constexpr std::size_t kMaxQueueSize = 4096;

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

    /// Drain all queued tasks on the calling thread (must be the UI thread). Tasks are moved
    /// out of the queue and each `Task` is released after invocation so captures (especially
    /// shared_ptr / large state) do not live across the whole drain loop.
    ///
    /// Pillar 1 + 2 perf-review (slice 2 of `docs/design/pillar-1-2-perf-review-system.md`):
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
        lastDrainTaskCount_.store(tasks.size(), std::memory_order_release);
        for (auto& t : tasks) {
            if (t) {
                t();
                t = nullptr; // release captures eagerly; previous code held them across the loop
            }
        }
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

  private:
    std::mutex mutex_;
    std::vector<Task> queue_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<std::size_t> lastDrainTaskCount_{0};
};
