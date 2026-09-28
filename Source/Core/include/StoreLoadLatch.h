#pragma once

// StoreLoadLatch — "load the saved rows once per backend" bookkeeping shared by the offline lookup
// services (Quality Pillar 6). A key is claimed once. A failed load releases the claim with a backoff,
// so a later call retries without launching a worker every frame while the store keeps failing.
// Thread-safe; the mutex is never held while a load runs. LoadSavedRowsOnce is the one way the
// services drive it.

#include "Logger.h"
#include "OfflineFirstPure.h"
#include "ScopeExit.h"

#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace smatchet {
namespace offline {

class StoreLoadLatch {
  public:
    explicit StoreLoadLatch(int retryAfterSeconds = kLookupRetryAfterSeconds) : retryAfterSeconds_(retryAfterSeconds) {}
    StoreLoadLatch(const StoreLoadLatch&) = delete;
    StoreLoadLatch& operator=(const StoreLoadLatch&) = delete;

    /// True when the caller now owns the load for `key`; it must then call MarkLoaded or MarkFailed.
    /// False while the key is claimed (loaded or loading) or inside the backoff after a failed load.
    bool TryClaim(const std::string& key, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claimed_.find(key) != claimed_.end()) {
            return false;
        }
        const auto retryIt = retryAfter_.find(key);
        if (retryIt != retryAfter_.end() && now < retryIt->second) {
            return false;
        }
        claimed_.insert(key);
        return true;
    }

    void MarkLoaded(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        retryAfter_.erase(key);
    }

    /// Release the claim; the next TryClaim for `key` succeeds once the backoff has passed.
    void MarkFailed(const std::string& key, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        claimed_.erase(key);
        retryAfter_[key] = now + std::chrono::seconds(retryAfterSeconds_);
    }

    /// Forget every backoff (e.g. connectivity came back) so released keys can be claimed at once.
    void ClearBackoff() {
        std::lock_guard<std::mutex> lock(mutex_);
        retryAfter_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> claimed_;
    std::unordered_map<std::string, Clock::time_point> retryAfter_;
    int retryAfterSeconds_;
};

/// Load the saved rows of `kind` for `backendKey` once, on a worker. `deps` supplies LookupCacheShared()
/// and LaunchBackgroundTask(std::function<void()>). On the worker, `apply(backendKey, rows)` gets the rows
/// read. No store yet claims nothing, so a later call tries again. A failed read (a throw from the store
/// or from `apply`) releases the claim with a backoff. A launch that throws releases it too and is
/// logged under `what` (e.g. "saved components"). Returns false only in that last case.
template <typename Deps, typename ApplyFn>
bool LoadSavedRowsOnce(Deps& deps, StoreLoadLatch& latch, const std::string& backendKey, const char* kind,
                       const char* what, ApplyFn apply) {
    const auto store = deps.LookupCacheShared();
    if (!store || !latch.TryClaim(backendKey, Clock::now())) {
        return true; // no store yet, or loaded, loading, or backing off after a failed load
    }
    try {
        deps.LaunchBackgroundTask([&latch, store, backendKey, kind, apply]() {
            bool loaded = false;
            ScopeExit releaseOnFailure([&latch, &backendKey, &loaded]() {
                if (!loaded) {
                    latch.MarkFailed(backendKey, Clock::now());
                }
            });
            apply(backendKey, store->LoadLookups(backendKey, kind));
            loaded = true;
            latch.MarkLoaded(backendKey);
        });
    } catch (const std::exception& ex) {
        latch.MarkFailed(backendKey, Clock::now());
        LOG_WARN("Loading %s did not start: %s", what, ex.what());
        return false;
    }
    return true;
}

} // namespace offline
} // namespace smatchet
