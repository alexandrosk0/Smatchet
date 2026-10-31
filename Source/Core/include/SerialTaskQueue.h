#pragma once

// SerialTaskQueue — runs posted tasks one at a time, in the order they were posted, on workers started
// through a launcher. Used where writes must leave the UI thread (Quality Pillar 2) but must not reorder,
// e.g. two quick saves of the same ticket. A post made while a drain runs is picked up by that drain; an
// idle queue starts one. Tasks are expected to handle their own errors: one that throws ends its drain,
// and the rest run on the next post. Thread-safe. Lifetime: the owner must outlive every drain it started.
// `Flush` blocks until every posted task has run (tests, shutdown); never call it from a render path.

#include "ScopeExit.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace smatchet {

class SerialTaskQueue {
  public:
    using Launcher = std::function<void(std::function<void()>)>;

    explicit SerialTaskQueue(Launcher launch) : launch_(std::move(launch)) {}
    SerialTaskQueue(const SerialTaskQueue&) = delete;
    SerialTaskQueue& operator=(const SerialTaskQueue&) = delete;

    /// Any thread. Returns the launch failure message, or "". On a failed launch the task stays queued and
    /// runs after the next successful post.
    std::string Post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
            if (draining_) {
                return std::string();
            }
            draining_ = true;
        }
        try {
            launch_([this]() { Drain(); });
        } catch (const std::exception& ex) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                draining_ = false;
            }
            idle_.notify_all();
            return std::string(ex.what());
        }
        return std::string();
    }

    /// Blocks until every task posted so far has run, or `timeout` passes (true: the queue is idle). A task
    /// stranded by a failed launch runs on the calling thread. Tests and the owner's shutdown only: it parks
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
    void Drain() {
        // Released here only when a task throws; the normal exit releases it under the lock.
        bool released = false;
        ScopeExit release([this, &released]() {
            if (!released) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    draining_ = false;
                }
                idle_.notify_all();
            }
        });
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) {
                    draining_ = false;
                    released = true;
                    idle_.notify_all();
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    Launcher launch_;
    std::mutex mutex_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> tasks_;
    bool draining_ = false;
};

} // namespace smatchet
