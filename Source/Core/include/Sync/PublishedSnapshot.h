#pragma once

// PublishedSnapshot<T> — the in-memory view of SQLite-backed queue state that the UI thread reads instead
// of the database (Quality Pillar 2). Workers rebuild it after every change; the UI reads it with one
// atomic shared_ptr load and never waits on a lock.
//
// - Rebuilds are serialised, so an older read never overwrites a newer one, and a rebuild whose source
//   is no longer current (the cache file was replaced meanwhile) is dropped.
// - Requests are single-flight: a request made while a rebuild runs makes that rebuild run once more,
//   so the last request always ends in a view read after it.
// - Thread-safe. Lifetime: the owner must outlive every rebuild it launched (the queue services' owner
//   joins its background tasks before destroying them).

#include "ScopeExit.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace smatchet {

template <typename T> class PublishedSnapshot {
  public:
    using Clock = std::chrono::steady_clock;

    PublishedSnapshot() : view_(std::make_shared<const T>()) {}
    PublishedSnapshot(const PublishedSnapshot&) = delete;
    PublishedSnapshot& operator=(const PublishedSnapshot&) = delete;

    /// The last published view; never null. Any thread.
    std::shared_ptr<const T> Get() const { return std::atomic_load(&view_); }

    /// True once a view has been published since construction or the last Invalidate().
    bool Loaded() const { return loaded_.load(); }

    /// Worker: fill a fresh T with `read(T&)` and publish it unless `isCurrent()` is false by then.
    /// Returns the read's error message, or "" (published, or dropped as no longer current); on an error
    /// the previous view stays. Any exception from `read` is reported, not thrown.
    template <typename ReadFn, typename IsCurrentFn> std::string Rebuild(ReadFn&& read, IsCurrentFn&& isCurrent) {
        std::lock_guard<std::mutex> lock(rebuildMutex_);
        std::shared_ptr<T> next;
        try {
            next = std::make_shared<T>();
            read(*next);
        } catch (const std::exception& ex) {
            return std::string(ex.what());
        } catch (...) {
            return std::string("unknown exception");
        }
        if (!isCurrent()) {
            return std::string();
        }
        std::atomic_store(&view_, std::shared_ptr<const T>(std::move(next)));
        loaded_.store(true);
        return std::string();
    }

    /// Run `rebuild` on a worker started through `launch(std::function<void()>)`. Unless `force`, a request
    /// within `backoff` of the previous unforced start is dropped (a failing first load retries slowly).
    /// Returns the launch failure message, or "".
    template <typename LaunchFn>
    std::string RequestRebuild(LaunchFn&& launch, std::function<void()> rebuild, bool force,
                               std::chrono::seconds backoff) {
        if (!force) {
            std::lock_guard<std::mutex> lock(scheduleMutex_);
            const Clock::time_point now = Clock::now();
            if (now < nextUnforcedAt_) {
                return std::string();
            }
            nextUnforcedAt_ = now + backoff;
        }
        again_.store(true);
        if (running_.exchange(true)) {
            return std::string(); // the running rebuild sees again_ and runs once more
        }
        try {
            launch([this, rebuild]() { RunRebuilds(rebuild); });
        } catch (const std::exception& ex) {
            running_.store(false);
            return std::string(ex.what());
        }
        return std::string();
    }

    /// The view no longer matches its source (e.g. the cache file was replaced): Loaded() reads false and
    /// the next unforced request starts at once. The old view stays readable until a rebuild replaces it.
    void Invalidate() {
        {
            std::lock_guard<std::mutex> lock(scheduleMutex_);
            nextUnforcedAt_ = Clock::time_point();
        }
        loaded_.store(false);
    }

  private:
    void RunRebuilds(const std::function<void()>& rebuild) {
        // Released here only when `rebuild` throws; every normal exit has already settled running_.
        bool settled = false;
        ScopeExit release([this, &settled]() {
            if (!settled) {
                running_.store(false);
            }
        });
        for (;;) {
            while (again_.exchange(false)) {
                rebuild();
            }
            running_.store(false);
            // A request between the last exchange and the store above saw running_ still true and left
            // again_ set: run it here unless a newly launched worker has already claimed it.
            if (!again_.load() || running_.exchange(true)) {
                settled = true;
                return;
            }
        }
    }

    std::shared_ptr<const T> view_; ///< std::atomic_load / std::atomic_store only
    std::mutex rebuildMutex_;
    std::atomic<bool> loaded_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> again_{false};
    std::mutex scheduleMutex_;
    Clock::time_point nextUnforcedAt_{};
};

} // namespace smatchet
