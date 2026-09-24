#pragma once

// KeyedLookupCache — the one offline-aware state machine for a keyed, network-backed lookup (Quality
// Pillar 6). It replaces hand-rolled inFlight / loaded / retryAfter trios.
//
// Rules it enforces: a failed fetch never marks a key loaded and never discards a value that was
// already there; it records a retry-after deadline instead. No fetch starts while the tracker is
// offline or inside the backoff window. RunKeyedFetch clears InFlight on every exit path, including a
// throw. The mutex is never held while a fetch runs. Thread-safe.

#include "OfflineFirstPure.h"
#include "ScopeExit.h"
#include "Tracker/TrackerError.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace smatchet {
namespace offline {

template <typename Value> class KeyedLookupCache {
  public:
    struct Entry {
        bool HasValue = false;
        bool Live = false;
        bool InFlight = false;
        bool LastAttemptFailed = false;
        TrackerErrorKind LastErrorKind = TrackerErrorKind::None;
        std::string LastError;
        Value Payload{};
    };
    struct Ticket {
        std::string Key;
        std::uint64_t Gen = 0;
    };

    explicit KeyedLookupCache(int retryAfterSeconds = kLookupRetryAfterSeconds) : retryAfterSeconds_(retryAfterSeconds) {}

    Entry Get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(key);
        return it == slots_.end() ? Entry() : it->second.E;
    }

    DataFreshness Freshness(const std::string& key, TrackerConnectivityState connectivity) const {
        const Entry e = Get(key);
        FreshnessInputs in;
        in.HasCache = e.HasValue;
        in.Live = e.Live;
        in.InFlight = e.InFlight;
        in.LastAttemptFailed = e.LastAttemptFailed;
        in.Connectivity = connectivity;
        return ClassifyFreshness(in);
    }

    /// True (and `out` filled) when the caller should launch a fetch for `key` now.
    bool TryBeginFetch(const std::string& key, TrackerConnectivityState connectivity, Clock::time_point now,
                       Ticket& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& s = slots_[key];
        if (s.E.InFlight || (s.E.Live && !s.E.LastAttemptFailed)) {
            return false;
        }
        if (!ShouldAttemptNetwork(connectivity, now, s.RetryAfter)) {
            return false;
        }
        s.E.InFlight = true;
        s.Gen = ++nextGen_;
        out.Key = key;
        out.Gen = s.Gen;
        return true;
    }

    void CompleteSuccess(const Ticket& t, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(t.Key);
        if (it == slots_.end() || it->second.Gen != t.Gen) {
            return; // invalidated while in flight — drop the stale result
        }
        Entry& e = it->second.E;
        e.Payload = std::move(value);
        e.HasValue = true;
        e.Live = true;
        e.InFlight = false;
        e.LastAttemptFailed = false;
        e.LastErrorKind = TrackerErrorKind::None;
        e.LastError.clear();
        it->second.RetryAfter = Clock::time_point();
    }

    void CompleteFailure(const Ticket& t, const TrackerError& error, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(t.Key);
        if (it == slots_.end() || it->second.Gen != t.Gen) {
            return;
        }
        Entry& e = it->second.E;
        e.InFlight = false;
        e.LastAttemptFailed = true;
        e.LastErrorKind = error.Kind;
        e.LastError = error.Detail;
        it->second.RetryAfter = now + std::chrono::seconds(retryAfterSeconds_);
    }

    /// Seed a value restored from local storage. Never overrides a live value.
    void SeedFromStore(const std::string& key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = slots_[key].E;
        if (e.Live) {
            return;
        }
        e.Payload = std::move(value);
        e.HasValue = true;
    }

    /// Forget one key, e.g. after a successful write changed what the lookup returns.
    void Invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.erase(key);
    }

    /// Connectivity came back: clear every backoff so failed keys retry on their next use.
    void OnConnectivityRecovered() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : slots_) {
            kv.second.RetryAfter = Clock::time_point();
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
    }

  private:
    struct Slot {
        Entry E;
        Clock::time_point RetryAfter{};
        std::uint64_t Gen = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> slots_;
    std::uint64_t nextGen_ = 0;
    int retryAfterSeconds_;
};

/// Run `fetch` (returns Result<Value, TrackerError>) for a ticket from TryBeginFetch and record the
/// outcome. Call it on a worker thread. A throw is recorded as an Unknown failure (and rethrown), so
/// InFlight can never stay latched.
template <typename Value, typename FetchFn>
void RunKeyedFetch(KeyedLookupCache<Value>& cache, const typename KeyedLookupCache<Value>::Ticket& ticket,
                   FetchFn&& fetch) {
    bool recorded = false;
    ScopeExit recordThrow([&cache, &ticket, &recorded]() {
        if (!recorded) {
            cache.CompleteFailure(ticket, TrackerErrorUnknown("lookup fetch threw"), Clock::now());
        }
    });
    auto result = fetch();
    recorded = true;
    if (result.has_value()) {
        cache.CompleteSuccess(ticket, std::move(result.value()));
    } else {
        cache.CompleteFailure(ticket, result.error(), Clock::now());
    }
}

} // namespace offline
} // namespace smatchet
