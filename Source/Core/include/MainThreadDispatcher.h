#pragma once

#include "Logger.h"
#include "MainThreadDispatcherDrain.h"
#include "UiPerfMonitor.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

/// Bounded, thread-safe queue of tasks drained once per frame on the main (UI) thread.
/// Workers post lambdas instead of setting ad-hoc one-shot atomic/bool flags, centralising
/// the UI-thread-callback contract (BACKLOG_CODE_REVIEW.md §6.1). Drain is called at the head of
/// SmatchetUI::Draw so tasks execute before any window drawing begins that frame.
/// Lifetime contract: the dispatcher is typically a member of `AppController`. Callers MUST
/// stop posting (via `BeginShutdown()`) and join all worker threads before `~AppController`
/// runs — `BeginShutdown()` flips a shutdown atom so late posts no-op rather than touching
/// the about-to-be-destroyed mutex.
/// Bound: `kMaxQueueSize` droppable tasks plus `kMaxCompletionQueueSize` completions. On overflow the
/// oldest droppable task is dropped, never a completion. Every lost task is counted
/// (`DroppedTaskCount()`) and logged, at most one line a second per severity. A post never throws:
/// a queue that cannot grow loses that one task, counted and logged the same way.
class MainThreadDispatcher {
  public:
    using Task = std::function<void()>;

    /// Maximum number of pending droppable tasks (`PostToMainThread`). Posts beyond this drop the
    /// oldest pending droppable task so a runaway producer cannot grow memory without bound. 4096
    /// mirrors Logger's file-sink bound and is well above any reasonable per-frame burst.
    static constexpr std::size_t kMaxQueueSize = 4096;

    /// Maximum number of pending completions (`PostCompletionToMainThread`), counted separately so
    /// droppable traffic can never push a completion out. Reaching it means the UI thread has stopped
    /// draining; the oldest completion is then dropped, counted and logged as an error.
    static constexpr std::size_t kMaxCompletionQueueSize = 4096;

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
    /// thread; never throws. No-ops if `BeginShutdown()` has been called. Droppable: at
    /// `kMaxQueueSize` pending droppable tasks the oldest of them is dropped.
    void PostToMainThread(Task t) { Post(std::move(t), /*completion=*/false); }

    /// Post a completion: the task that releases a caller's in-flight latch (a Save / Post / commit
    /// button) and so must run for the UI to recover. Same contract as `PostToMainThread`, except
    /// that an overflow of droppable tasks never evicts it; see `kMaxCompletionQueueSize` for its own
    /// cap. Drain order is unchanged: completions and droppable tasks run FIFO together.
    void PostCompletionToMainThread(Task t) { Post(std::move(t), /*completion=*/true); }

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
        std::vector<Entry> tasks;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tasks.swap(queue_);
            completionsQueued_ = 0;
        }
        const bool budgeted = !shuttingDown_.load(std::memory_order_acquire);
        const auto deadline = std::chrono::steady_clock::now() + DrainBudget();
        std::size_t ran = 0;
        std::size_t deferred = 0;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].Fn) {
                tasks[i].Fn();
                tasks[i].Fn = nullptr; // release captures eagerly; previous code held them across the loop
                ++ran;
            }
            // Check the clock only after running at least one task (always make forward progress)
            // and only when work remains. During shutdown the budget is ignored so the final drain
            // empties the queue rather than orphaning the tail.
            if (budgeted && i + 1 < tasks.size() && std::chrono::steady_clock::now() >= deadline) {
                std::vector<Entry> tail;
                tail.reserve(tasks.size() - (i + 1));
                for (std::size_t j = i + 1; j < tasks.size(); ++j) {
                    tail.push_back(std::move(tasks[j]));
                }
                deferred = tail.size();
                smatchet::DispatcherQueueTrim trim;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    std::vector<Entry> merged;
                    smatchet::RequeueDeferredFront(tail, queue_, (std::numeric_limits<std::size_t>::max)(), merged);
                    trim = smatchet::TrimDispatcherQueue(merged, kMaxQueueSize, kMaxCompletionQueueSize,
                                                         &MainThreadDispatcher::IsCompletion);
                    completionsQueued_ = trim.CompletionsLeft;
                    queue_.swap(merged);
                }
                ReportEvicted(trim);
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

    /// Tasks lost since construction: evicted at a cap, or not queued because the queue could not
    /// grow. Any thread. Each loss is also logged (rate-limited); this is the exact total.
    std::uint64_t DroppedTaskCount() const noexcept { return droppedTasks_.load(std::memory_order_acquire); }

  private:
    struct Entry {
        Entry(Task fn, bool completion) : Fn(std::move(fn)), Completion(completion) {}
        Task Fn;
        bool Completion;
    };

    static bool IsCompletion(const Entry& e) { return e.Completion; }

    /// A saturated queue loses tasks on every post; log a line a second per severity, not one a post.
    static constexpr std::int64_t kLossLogIntervalNs = 1000000000;
    static constexpr std::int64_t kNeverLogged = (std::numeric_limits<std::int64_t>::min)();

    void Post(Task t, bool completion) {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        smatchet::DispatcherQueueTrim trim;
        bool queued = false;
        char failure[128] = {};
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (shuttingDown_.load(std::memory_order_relaxed)) {
                // Re-check under the lock so a teardown that started between the unlocked check
                // and the lock acquisition does not see a partially-posted task.
                return;
            }
            try {
                queue_.emplace_back(std::move(t), completion);
                queued = true;
            } catch (const std::exception& ex) {
                // Typically std::bad_alloc. The queue is unchanged (strong guarantee); the task is lost.
                CopyReason(failure, sizeof(failure), ex.what());
            } catch (...) {
                CopyReason(failure, sizeof(failure), "unknown exception");
            }
            if (queued) {
                completionsQueued_ += completion ? 1 : 0;
                // The trim scans the queue, so it runs only past a cap, which is pathological.
                if (queue_.size() - completionsQueued_ > kMaxQueueSize ||
                    completionsQueued_ > kMaxCompletionQueueSize) {
                    trim = smatchet::TrimDispatcherQueue(queue_, kMaxQueueSize, kMaxCompletionQueueSize,
                                                         &MainThreadDispatcher::IsCompletion);
                    completionsQueued_ = trim.CompletionsLeft;
                }
            }
        }
        // Report outside the lock: logging allocates and must not stall other posters.
        if (!queued) {
            const std::uint64_t total = droppedTasks_.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (ShouldLogLoss(lastErrorLogNs_)) {
                try {
                    LOG_ERROR("MainThreadDispatcher: could not queue a %s task (%s); it will not run (%llu task(s) "
                              "lost so far)",
                              completion ? "completion" : "droppable", failure, static_cast<unsigned long long>(total));
                } catch (...) {
                    // catch-all-ok: the loss is already counted in DroppedTaskCount(); formatting the line can
                    // itself fail while memory is exhausted, and a post must never throw.
                }
            }
        }
        ReportEvicted(trim);
    }

    void ReportEvicted(const smatchet::DispatcherQueueTrim& trim) {
        const std::size_t lost = trim.DroppedDroppable + trim.DroppedCompletions;
        if (lost == 0) {
            return;
        }
        const std::uint64_t total = droppedTasks_.fetch_add(lost, std::memory_order_acq_rel) + lost;
        const bool completionLost = trim.DroppedCompletions > 0;
        if (!ShouldLogLoss(completionLost ? lastErrorLogNs_ : lastWarnLogNs_)) {
            return;
        }
        try {
            if (completionLost) {
                LOG_ERROR("MainThreadDispatcher: completion queue full, dropped the oldest %zu completion(s) and %zu "
                          "droppable task(s); the UI thread is not draining (%llu task(s) lost so far)",
                          trim.DroppedCompletions, trim.DroppedDroppable, static_cast<unsigned long long>(total));
            } else {
                LOG_WARN("MainThreadDispatcher: queue full, dropped the oldest %zu droppable task(s) (%llu task(s) "
                         "lost so far)",
                         trim.DroppedDroppable, static_cast<unsigned long long>(total));
            }
        } catch (...) {
            // catch-all-ok: the loss is already counted in DroppedTaskCount(); a post or drain must not throw
            // because its log line could not be formatted.
        }
    }

    /// True at most once per `kLossLogIntervalNs` for one limiter (first loss always logs).
    static bool ShouldLogLoss(std::atomic<std::int64_t>& lastLogNs) {
        const std::int64_t now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        std::int64_t last = lastLogNs.load(std::memory_order_relaxed);
        if (last != kNeverLogged && now - last < kLossLogIntervalNs) {
            return false;
        }
        return lastLogNs.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }

    /// Copies `reason` into `out` without allocating (it runs right after an allocation failed).
    static void CopyReason(char* out, std::size_t outSize, const char* reason) {
        std::size_t i = 0;
        for (; reason != nullptr && reason[i] != '\0' && i + 1 < outSize; ++i) {
            out[i] = reason[i];
        }
        out[i] = '\0';
    }

    mutable std::mutex mutex_;
    std::vector<Entry> queue_;
    std::size_t completionsQueued_ = 0; ///< completions in queue_; guarded by mutex_
    std::atomic<bool> shuttingDown_{false};
    std::atomic<std::size_t> lastDrainTaskCount_{0};
    std::atomic<std::size_t> lastDrainDeferredCount_{0};
    std::atomic<std::uint64_t> droppedTasks_{0};
    std::atomic<std::int64_t> lastWarnLogNs_{kNeverLogged};
    std::atomic<std::int64_t> lastErrorLogNs_{kNeverLogged};
};
