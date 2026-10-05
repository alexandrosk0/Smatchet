#pragma once

// SourcedSnapshot<T, Source> — a PublishedSnapshot<T> filled from a source that can be replaced while the
// app runs: the offline-queue services read their SQLite tables through the deps' current cache, which a
// cache rebuild swaps. OfflineQueueService and PendingActionQueueService share it, so the read, publish
// and relaunch steps exist once (Quality Pillar 2: the UI reads only the published view).
//
// - Publish(source) reads `source` on the calling worker and publishes the result, unless `current()` no
//   longer returns that source by then.
// - RequestAsync() runs Publish on a worker, for whatever source is current when the worker runs.
// Thread-safe. Lifetime: the owner outlives every rebuild it launched (see PublishedSnapshot).

#include "Logger.h"
#include "Sync/PublishedSnapshot.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace smatchet {

template <typename T, typename Source> class SourcedSnapshot {
  public:
    using CurrentFn = std::function<std::shared_ptr<Source>()>;
    using LaunchFn = std::function<void(std::function<void()>)>;
    using FillFn = std::function<void(Source&, T&)>;

    /// `owner` (a string literal) prefixes log lines; `current` returns the source to read now (null when
    /// there is none); `launch` starts a worker; `fill` reads a source into a fresh view and may throw.
    SourcedSnapshot(const char* owner, CurrentFn current, LaunchFn launch, FillFn fill)
        : owner_(owner), current_(std::move(current)), launch_(std::move(launch)), fill_(std::move(fill)) {}
    /// Bound to `deps` (the offline-queue deps: CacheShared() + LaunchBackgroundTask()), which must outlive
    /// this.
    template <typename Deps>
    SourcedSnapshot(const char* owner, Deps& deps, FillFn fill)
        : SourcedSnapshot(
              owner, [&deps]() { return deps.CacheShared(); },
              [&deps](std::function<void()> task) { deps.LaunchBackgroundTask(std::move(task)); }, std::move(fill)) {}
    SourcedSnapshot(const SourcedSnapshot&) = delete;
    SourcedSnapshot& operator=(const SourcedSnapshot&) = delete;

    /// The last published view; never null. Any thread.
    std::shared_ptr<const T> Get() const { return snapshot_.Get(); }
    bool Loaded() const { return snapshot_.Loaded(); }
    void Invalidate() { snapshot_.Invalidate(); }

    /// Worker: read `source` into a new view and publish it. A failed read keeps the previous view.
    void Publish(Source& source) {
        const std::string error = snapshot_.Rebuild([this, &source](T& next) { fill_(source, next); },
                                                    [this, &source]() { return current_().get() == &source; });
        if (!error.empty()) {
            LOG_WARN("%s: reading the queue failed; keeping the previous view: %s", owner_, error.c_str());
        }
    }

    /// Any thread: Publish the source current when the worker runs. Unless `force`, a request within
    /// `backoff` of the previous unforced one is dropped (a failing first load retries slowly).
    void RequestAsync(bool force, std::chrono::seconds backoff) {
        const std::string error = snapshot_.RequestRebuild(
            launch_,
            [this]() {
                const std::shared_ptr<Source> source = current_();
                if (source) {
                    Publish(*source);
                }
            },
            force, backoff);
        if (!error.empty()) {
            LOG_WARN("%s: could not start reading the queue: %s", owner_, error.c_str());
        }
    }

  private:
    const char* owner_;
    CurrentFn current_;
    LaunchFn launch_;
    FillFn fill_;
    PublishedSnapshot<T> snapshot_;
};

} // namespace smatchet
