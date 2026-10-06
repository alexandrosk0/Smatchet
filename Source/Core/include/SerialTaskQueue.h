#pragma once

// SerialTaskQueue — runs posted tasks one at a time, in the order they were posted, on workers started
// through a launcher. Used where writes must leave the UI thread (Quality Pillar 2) but must not reorder,
// e.g. two quick saves of the same ticket. A post made while a drain runs is picked up by that drain; an
// idle queue starts one. Tasks are expected to handle their own errors: one that throws ends its drain,
// and the rest run on the next post. Thread-safe. Lifetime: the owner must outlive every drain it started.

#include "ScopeExit.h"

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
            std::lock_guard<std::mutex> lock(mutex_);
            draining_ = false;
            return std::string(ex.what());
        }
        return std::string();
    }

  private:
    void Drain() {
        // Released here only when a task throws; the normal exit releases it under the lock.
        bool released = false;
        ScopeExit release([this, &released]() {
            if (!released) {
                std::lock_guard<std::mutex> lock(mutex_);
                draining_ = false;
            }
        });
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) {
                    draining_ = false;
                    released = true;
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
    std::deque<std::function<void()>> tasks_;
    bool draining_ = false;
};

} // namespace smatchet
