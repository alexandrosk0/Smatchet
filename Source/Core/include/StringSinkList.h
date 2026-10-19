#pragma once

// StringSinkList — `void(const std::string&)` callbacks registered from one thread (plugin init, a
// command on the UI thread) and invoked from others (the automation worker, the UI thread).
//
// Copy-on-write: Add and Clear publish a new immutable vector under a mutex, and Snapshot hands out
// the current one by shared_ptr. An invocation therefore iterates a vector no other thread can
// change or reallocate under it, and calls the sinks without holding the lock, so a sink may itself
// register a sink, log, or post without deadlocking. Registration is rare and invocation frequent,
// which is the trade this makes: an Add copies the list, a Snapshot only bumps a reference count.

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

    StringSinkList() : current_(EmptySinks()) {}
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

    /// Drop every sink. An invocation already holding a snapshot finishes with the old list; the
    /// sinks (and their captures) are destroyed when the last snapshot goes, outside this lock.
    void Clear() {
        std::shared_ptr<const Sinks> old = EmptySinks();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.swap(old);
        }
    }

    /// The current sinks; never null. Iterate it freely: the vector is never modified after publication.
    std::shared_ptr<const Sinks> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    /// Call every current sink with `message`, in registration order, without holding the lock.
    /// Returns false when there was no sink to call.
    bool InvokeAll(const std::string& message) const {
        const std::shared_ptr<const Sinks> sinks = Snapshot();
        for (const Sink& sink : *sinks) {
            sink(message);
        }
        return !sinks->empty();
    }

  private:
    /// One shared empty list, so Clear (a teardown step) never allocates and so never throws. Leaked on
    /// purpose: a static owner destroyed after this function's statics could otherwise Clear() into a
    /// destroyed shared_ptr.
    static std::shared_ptr<const Sinks> EmptySinks() {
        static const std::shared_ptr<const Sinks>* const empty =
            new std::shared_ptr<const Sinks>(std::make_shared<const Sinks>());
        return *empty;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<const Sinks> current_;
};

} // namespace smatchet
