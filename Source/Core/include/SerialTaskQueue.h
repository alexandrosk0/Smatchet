#pragma once

// SerialTaskQueue — runs posted tasks one at a time, in the order they were posted, on workers started
// through a launcher. Used where writes must leave the UI thread (Quality Pillar 2) but must not reorder,
// e.g. two quick saves of the same ticket. A post made while a drain runs is picked up by that drain; an
// idle queue starts one. Tasks are expected to handle their own errors: one that throws ends its drain,
// and the rest run on the next post. Thread-safe. Lifetime: the owner must outlive every drain it started.
// `Flush` blocks until every posted task has run (tests, shutdown); never call it from a render path.
//
// The "a drain is running" latch is owned by a LaunchClaim, so it is released exactly once however the
// launch ends: the drain ran, the launch threw, or the launcher dropped the closure unrun (shutdown). A
// dropped drain leaves its tasks queued; the next Post relaunches, and `Flush` runs them on the caller.

#include "LaunchClaim.h"
#include "ScopeExit.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace smatchet {

/// Time left until `deadline`, never negative. Lets several queues share one flush budget (shutdown).
inline std::chrono::milliseconds RemainingUntil(std::chrono::steady_clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

class SerialTaskQueue {
  public:
    using Launcher = std::function<void(std::function<void()>)>;

    explicit SerialTaskQueue(Launcher launch) : launch_(std::move(launch)) {}
    SerialTaskQueue(const SerialTaskQueue&) = delete;
    SerialTaskQueue& operator=(const SerialTaskQueue&) = delete;

    /// Any thread. Returns the launch failure message, or "". On a failed launch the task stays queued and
    /// runs after the next successful post (or on `Flush`).
    std::string Post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
            if (draining_) {
                return std::string();
            }
            draining_ = true;
        }
        std::shared_ptr<LaunchClaim> claim;
        try {
            claim = std::make_shared<LaunchClaim>([this]() { ReleaseDrain_(); });
            launch_([this, claim]() {
                if (claim->TakeForRun()) {
                    Drain();
                }
            });
        } catch (const std::exception& ex) {
            ReleaseUnlaunchedDrain_(claim);
            return std::string(ex.what());
        } catch (...) {
            // Core logic tier: the latch must not stick; the unknown exception is the caller's to handle.
            ReleaseUnlaunchedDrain_(claim);
            throw;
        }
        return std::string();
    }

    /// Tasks not yet finished: the queued ones plus the one running. Any thread; for shutdown diagnostics.
    std::size_t PendingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size() + (running_ ? 1U : 0U);
    }

    /// Blocks until every task posted so far has run, or `timeout` passes (true: the queue is idle). `timeout`
    /// bounds the wait for a drain another thread is running; a task stranded by a failed launch runs on the
    /// calling thread to completion, whatever the budget. Tests and the owner's shutdown only: it parks
    /// the caller for as long as the tasks take.
    bool Flush(std::chrono::milliseconds timeout = std::chrono::hours(24)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (!idle_.wait_until(lock, deadline, [this]() { return !draining_; })) {
                return false;
            }
            if (tasks_.empty()) {
                return true;
            }
            draining_ = true; // a failed launch left tasks behind: drain them here
            lock.unlock();
            Drain();
            lock.lock();
        }
    }

  private:
    void ReleaseDrain_() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            draining_ = false;
            running_ = false; // a task that threw ended its drain
        }
        idle_.notify_all();
    }

    // The launch ended without a drain running: release through the claim, or directly when the claim
    // itself could not be built.
    void ReleaseUnlaunchedDrain_(const std::shared_ptr<LaunchClaim>& claim) {
        if (claim) {
            claim->ReleaseIfUntaken();
        } else {
            ReleaseDrain_();
        }
    }

    void Drain() {
        // Released here only when a task throws; the normal exit releases it under the lock.
        bool released = false;
        ScopeExit release([this, &released]() {
            if (!released) {
                ReleaseDrain_();
            }
        });
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false; // the previous task finished
                if (tasks_.empty()) {
                    draining_ = false;
                    released = true;
                    idle_.notify_all();
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                running_ = true;
            }
            task();
        }
    }

    Launcher launch_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> tasks_;
    bool draining_ = false;
    bool running_ = false; ///< a popped task is executing (guarded by mutex_; counted by PendingCount)
};

} // namespace smatchet
