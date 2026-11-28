#pragma once

// KeyedLookupCache — the one offline-aware state machine for a keyed, network-backed lookup (Quality
// Pillar 6). It replaces hand-rolled inFlight / loaded / retryAfter trios.
//
// Rules it enforces: a failed fetch never marks a key loaded and never discards a value that was
// already there; it records a retry-after deadline instead. No fetch starts while the tracker is
// offline or inside the backoff window. RunKeyedFetch clears InFlight on every exit path, including a
// throw. The mutex is never held while a fetch runs. Thread-safe.
//
// Admission. By default a value that is Live is served until the process ends or a caller Invalidates it.
// A caller that needs a live value to be re-checked opts in with a FetchAdmission (an age limit, a forced
// revalidation) or a stale predicate (TryBeginFetchIf). A revalidation never costs the entry what it holds:
// the Payload, HasValue and Live flag stay put while it runs and when it fails, so the cue reads Refreshing
// and then CachedStale instead of dropping the value (Pillar 6: a failed fetch never wipes cached data).

#include "Config/TunableLimits.h"
#include "OfflineFirstPure.h"
#include "ScopeExit.h"
#include "TrackerError.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace smatchet {
namespace offline {

/// When a Live value may be fetched again. The default (no age limit, no revalidation) keeps the original
/// rule: a Live value is never fetched again until it is invalidated.
struct FetchAdmission {
    /// A Live value older than this may be fetched again. Zero or negative disables the age limit.
    std::chrono::seconds LiveMaxAge{0};
    /// The caller wants a live answer now (a user's Refresh): no Live value counts as current. It still waits
    /// for connectivity and the failure backoff, and never starts a second fetch for a key already in flight.
    bool Revalidate = false;
};

/// The admission the revalidating callers use: a live value older than `lookup_live_max_age_sec` (0 = never)
/// may be fetched again, and `revalidate` asks for it whatever its age. Reads the tunable limits, so call it where a
/// lookup starts, not in a loop.
inline FetchAdmission LiveMaxAgeAdmission(bool revalidate) {
    FetchAdmission admit;
    admit.LiveMaxAge = std::chrono::seconds(limits::Get().LookupLiveMaxAgeSec);
    admit.Revalidate = revalidate;
    return admit;
}

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
        /// The entry held a Live value when this fetch was admitted (a revalidation). Its saved-copy shortcut
        /// must not run: a saved copy would replace the live value with an older one.
        bool WasLive = false;
    };

    explicit KeyedLookupCache(int retryAfterSeconds = kLookupRetryAfterSeconds)
        : retryAfterSeconds_(retryAfterSeconds) {}

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

    /// True (and `out` filled) when the caller should launch a fetch for `key` now. A key that is in flight is
    /// never admitted. A Live value is admitted only when `admit` asks for it (see FetchAdmission).
    bool TryBeginFetch(const std::string& key, TrackerConnectivityState connectivity, Clock::time_point now,
                       Ticket& out, const FetchAdmission& admit = FetchAdmission()) {
        return TryBeginFetchIf(key, connectivity, now, out, admit, [](const Value&) { return false; });
    }

    /// TryBeginFetch with a stale predicate: `liveIsStale(const Value& livePayload)` returning true makes a Live
    /// value admissible again (e.g. it answers a different question than the caller now asks). The predicate
    /// runs under the cache's mutex: keep it cheap, never throw, never call back into this cache.
    template <class StalePred>
    bool TryBeginFetchIf(const std::string& key, TrackerConnectivityState connectivity, Clock::time_point now,
                         Ticket& out, const FetchAdmission& admit, StalePred&& liveIsStale) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(key);
        if (it == slots_.end()) {
            if (!ShouldAttemptNetwork(connectivity, now, Clock::time_point())) {
                return false; // a refusal creates no slot, so an offline probe of a new key leaves nothing behind
            }
        } else {
            const Slot& existing = it->second;
            if (existing.E.InFlight) {
                return false;
            }
            if (existing.E.Live && !existing.E.LastAttemptFailed &&
                !(admit.Revalidate || !LiveIsCurrent(existing, now, admit) || liveIsStale(existing.E.Payload))) {
                return false;
            }
            if (!ShouldAttemptNetwork(connectivity, now, existing.RetryAfter)) {
                return false;
            }
        }
        Slot& s = slots_[key];
        s.E.InFlight = true;
        s.Gen = ++nextGen_;
        out.Key = key;
        out.Gen = s.Gen;
        out.WasLive = s.E.Live;
        return true;
    }

    /// True when `key` holds a Live value from a successful fetch that `admit` would still serve: no failed last
    /// attempt, no forced revalidation, and not older than `admit.LiveMaxAge`. Says nothing about an in-flight fetch.
    bool IsCurrent(const std::string& key, Clock::time_point now, const FetchAdmission& admit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(key);
        return it != slots_.end() && it->second.E.Live && !it->second.E.LastAttemptFailed && !admit.Revalidate &&
               LiveIsCurrent(it->second, now, admit);
    }

    void CompleteSuccess(const Ticket& t, Value value) { CompleteSuccess(t, std::move(value), Clock::now()); }

    /// `now` stamps the value's live time, the origin of FetchAdmission::LiveMaxAge.
    void CompleteSuccess(const Ticket& t, Value value, Clock::time_point now) {
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
        it->second.LiveAt = now;
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

    /// Ends the fetch `t` with a value served from local storage instead of the tracker (a saved copy still inside
    /// its lifetime): the entry holds it as a SAVED copy (not Live, so the cue says "saved") and no refetch starts
    /// before `retryAt`, when the saved copy expires. The ticket is spent: a completion recorded for it afterwards
    /// is ignored, so a caller that still records its normal success cannot turn the copy into a live value.
    void CompleteFromSaved(const Ticket& t, Value value, Clock::time_point retryAt) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(t.Key);
        if (it == slots_.end() || it->second.Gen != t.Gen) {
            return;
        }
        Entry& e = it->second.E;
        e.Payload = std::move(value);
        e.HasValue = true;
        e.Live = false;
        e.InFlight = false;
        e.LastAttemptFailed = false;
        e.LastErrorKind = TrackerErrorKind::None;
        e.LastError.clear();
        it->second.RetryAfter = retryAt;
        it->second.LiveAt = Clock::time_point();
        it->second.Gen = 0;
    }

    /// A user's Refresh: the next TryBeginFetch for `key` may run now, whatever failure backoff or saved-copy
    /// expiry was pending.
    void ClearBackoff(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(key);
        if (it != slots_.end()) {
            it->second.RetryAfter = Clock::time_point();
        }
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

    /// Drop every entry `keep(const std::string& key, const Entry& entry)` rejects, and every entry that holds no
    /// value and is not in flight (a failure record nothing is waiting on). An in-flight entry is never erased: its
    /// fetch still holds a ticket for it. Returns how many entries were erased. `keep` runs under the cache's mutex:
    /// keep it cheap, never throw, never call back into this cache.
    template <class KeepPred> std::size_t RetainIf(KeepPred&& keep) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t erased = 0;
        for (auto it = slots_.begin(); it != slots_.end();) {
            const Entry& e = it->second.E;
            if (!e.InFlight && (!e.HasValue || !keep(it->first, e))) {
                it = slots_.erase(it);
                ++erased;
            } else {
                ++it;
            }
        }
        return erased;
    }

    /// Number of entries held, in-flight and empty ones included.
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
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
        Clock::time_point LiveAt{}; ///< when the Live value was recorded (CompleteSuccess)
    };

    static bool LiveIsCurrent(const Slot& s, Clock::time_point now, const FetchAdmission& admit) {
        return admit.LiveMaxAge.count() <= 0 || now - s.LiveAt < admit.LiveMaxAge;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> slots_;
    std::uint64_t nextGen_ = 0;
    int retryAfterSeconds_;
};

/// Run `fetch` (returns Result<Value, TrackerError>) for a ticket from TryBeginFetch and record the
/// outcome. Call it on a worker thread. A throw from the fetch or from recording its result (e.g. a
/// throwing Value move-assignment) is recorded as an Unknown failure and rethrown, so InFlight can
/// never stay latched. `now()` (returns Clock::time_point) stamps the outcome; the overload without it
/// reads the steady clock.
template <typename Value, typename FetchFn, typename NowFn>
void RunKeyedFetch(KeyedLookupCache<Value>& cache, const typename KeyedLookupCache<Value>::Ticket& ticket,
                   FetchFn&& fetch, NowFn&& now) {
    bool recorded = false;
    ScopeExit recordThrow([&cache, &ticket, &recorded, &now]() {
        if (!recorded) {
            cache.CompleteFailure(ticket, TrackerErrorUnknown("lookup fetch threw"), now());
        }
    });
    auto result = fetch();
    if (result.has_value()) {
        cache.CompleteSuccess(ticket, std::move(result.value()), now());
    } else {
        cache.CompleteFailure(ticket, result.error(), now());
    }
    recorded = true; // only once a completion has actually been stored
}

template <typename Value, typename FetchFn>
void RunKeyedFetch(KeyedLookupCache<Value>& cache, const typename KeyedLookupCache<Value>::Ticket& ticket,
                   FetchFn&& fetch) {
    RunKeyedFetch(cache, ticket, std::forward<FetchFn>(fetch), []() { return Clock::now(); });
}

} // namespace offline
} // namespace smatchet
