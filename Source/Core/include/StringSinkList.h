#pragma once

// StringSinkList — `void(const std::string&)` callbacks registered from one thread (plugin init, a
// command on the UI thread) and invoked from others (the automation worker, the UI thread).
//
// Copy-on-write: Add and Clear publish a new immutable vector under a mutex, and Snapshot hands out
// the current one by shared_ptr. An invocation therefore iterates a vector no other thread can
// change or reallocate under it, and calls the sinks without holding the lock, so a sink may itself
// register a sink, log, or post without deadlocking. Registration is rare and invocation frequent,
// which is the trade this makes: an Add copies the list, a Snapshot only bumps a reference count.
// ClearAndWait additionally waits for running invocations, for teardown before the sinks' owners go.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {

class StringSinkList {
  public:
    using Sink = std::function<void(const std::string&)>;
    using Sinks = std::vector<Sink>;

    StringSinkList() : empty_(std::make_shared<const Sinks>()), current_(empty_) {}
    StringSinkList(const StringSinkList&) = delete;
    StringSinkList& operator=(const StringSinkList&) = delete;

    /// Append `sink` (an empty function is ignored). Safe against concurrent Snapshot / InvokeAll.
    void Add(Sink sink) {
        if (!sink) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto next = std::make_shared<Sinks>(*current_);
        next->push_back(std::move(sink));
        current_ = std::move(next);
    }

    /// Drop every sink without waiting. An invocation already running finishes with the old list; the
    /// sinks (and their captures) are destroyed when it ends. Never allocates, so never throws.
    void Clear() {
        std::shared_ptr<const Sinks> old = empty_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.swap(old);
        }
    }

    /// Drop every sink, then wait until no InvokeAll on another thread is still calling one. The
    /// teardown step before the sinks' owners (plugins capturing `this`) are destroyed: once this
    /// returns, no sink of the old list is running and none will be called again. An InvokeAll on the
    /// calling thread (this called from inside a sink) is not waited for, so that cannot deadlock.
    /// A sink must not block on the thread that calls this.
    void ClearAndWait() {
        std::shared_ptr<const Sinks> old = empty_;
        const std::size_t mine = InvocationsOnThisThread();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            current_.swap(old);
            idle_.wait(lock, [this, mine] { return inFlight_ <= mine; });
        }
    }

    /// The current sinks; never null. Iterate it freely: the vector is never modified after
    /// publication. A caller that runs the sinks should use InvokeAll instead: ClearAndWait does not
    /// see a snapshot taken here.
    std::shared_ptr<const Sinks> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    /// Call every current sink with `message`, in registration order, without holding the lock.
    /// Returns false when there was no sink to call.
    bool InvokeAll(const std::string& message) const {
        std::shared_ptr<const Sinks> sinks;
        const InFlight inFlight(*this, sinks);
        for (const Sink& sink : *sinks) {
            sink(message);
        }
        return !sinks->empty();
    }

  private:
    /// Marks one InvokeAll as running for its scope: counted under the lock (ClearAndWait waits for the
    /// count to drain) and recorded on the calling thread (so ClearAndWait from inside a sink skips it).
    class InFlight {
      public:
        InFlight(const StringSinkList& list, std::shared_ptr<const Sinks>& sinks) : list_(list) {
            ActiveOnThisThread().push_back(&list_);
            std::lock_guard<std::mutex> lock(list_.mutex_);
            sinks = list_.current_;
            ++list_.inFlight_;
        }
        ~InFlight() {
            {
                std::lock_guard<std::mutex> lock(list_.mutex_);
                --list_.inFlight_;
                // Every decrement, not only the last: a ClearAndWait called from inside a sink waits for the
                // count to fall to its own invocations (inFlight_ <= mine), which is above zero.
                list_.idle_.notify_all();
            }
            ActiveOnThisThread().pop_back();
        }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;

      private:
        const StringSinkList& list_;
    };

    static std::vector<const StringSinkList*>& ActiveOnThisThread() {
        static thread_local std::vector<const StringSinkList*> active;
        return active;
    }

    std::size_t InvocationsOnThisThread() const {
        const std::vector<const StringSinkList*>& active = ActiveOnThisThread();
        return static_cast<std::size_t>(std::count(active.begin(), active.end(), this));
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable idle_;
    mutable std::size_t inFlight_ = 0;
    const std::shared_ptr<const Sinks> empty_; ///< shared empty list: Clear never allocates
    std::shared_ptr<const Sinks> current_;
};

} // namespace smatchet
