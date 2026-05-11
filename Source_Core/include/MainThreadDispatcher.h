#pragma once

#include <functional>
#include <mutex>
#include <vector>

/// Bounded, thread-safe queue of tasks drained once per frame on the main (UI) thread.
///
/// Workers post lambdas instead of setting ad-hoc one-shot atomic/bool flags, centralising
/// the UI-thread-callback contract (CODE_REVIEW.md §6.1). Drain is called at the head of
/// SmatchetUI::Draw so tasks execute before any window drawing begins that frame.
///
/// Replaces the 8+ scattered deferred-notify booleans described in §6.1 incrementally:
/// new deferred work should go through PostToMainThread; existing flags are migrated over time.
class MainThreadDispatcher {
  public:
    using Task = std::function<void()>;

    void PostToMainThread(Task t) {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.push_back(std::move(t));
    }

    /// Drain all queued tasks on the calling thread (must be the UI thread).
    void Drain() {
        std::vector<Task> tasks;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            tasks.swap(queue_);
        }
        for (const auto& t : tasks) {
            t();
        }
    }

  private:
    std::mutex mutex_;
    std::vector<Task> queue_;
};
