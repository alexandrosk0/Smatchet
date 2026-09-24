#pragma once

// OfflineFirstPure — Quality Pillar 6 (offline-first) decisions shared by every network-backed read and
// every tracker write. Pure: no I/O, no Logger.h, no ImGui. See docs/agent-rules/quality-pillars.md § 6.

#include "Types/ConnectivityTypes.h"

#include <chrono>
#include <cstdint>

namespace smatchet {
namespace offline {

using Clock = std::chrono::steady_clock;

/// Backoff after a failed lookup fetch before the next attempt (matches the components loader).
constexpr int kLookupRetryAfterSeconds = 30;

/// True while the last connectivity probe says the tracker cannot be reached.
inline bool IsOfflineState(TrackerConnectivityState s) {
    return s == TrackerConnectivityState::TransportDown || s == TrackerConnectivityState::ServiceUnavailable;
}

/// A fetch may start only when the tracker is not known to be offline and any backoff has passed.
inline bool ShouldAttemptNetwork(TrackerConnectivityState s, Clock::time_point now, Clock::time_point retryAfter) {
    return !IsOfflineState(s) && now >= retryAfter;
}

enum class DataFreshness : std::uint8_t {
    Fresh,              ///< live data from this session and the tracker is reachable
    Refreshing,         ///< cached data shown while a live fetch runs
    CachedOffline,      ///< cached data shown (even this session's live data); the tracker is unreachable
    CachedStale,        ///< cached data shown; the last live fetch failed or has not run yet
    LoadingNoCache,     ///< nothing cached yet; a live fetch is running
    UnavailableNoCache, ///< nothing cached and no fetch running
};

struct FreshnessInputs {
    bool HasCache = false;          ///< a value (live or restored) is available to render
    bool Live = false;              ///< the value came from a successful fetch in this session
    bool InFlight = false;          ///< a fetch is running now
    bool LastAttemptFailed = false; ///< the most recent fetch failed
    TrackerConnectivityState Connectivity = TrackerConnectivityState::Unknown;
};

inline DataFreshness ClassifyFreshness(const FreshnessInputs& in) {
    if (!in.HasCache) {
        return in.InFlight ? DataFreshness::LoadingNoCache : DataFreshness::UnavailableNoCache;
    }
    if (in.InFlight) {
        return DataFreshness::Refreshing;
    }
    if (in.Live && !in.LastAttemptFailed && !IsOfflineState(in.Connectivity)) {
        return DataFreshness::Fresh;
    }
    return IsOfflineState(in.Connectivity) ? DataFreshness::CachedOffline : DataFreshness::CachedStale;
}

/// False only when there is nothing to show; every other state renders the (cached) content.
inline bool ShouldRenderContent(DataFreshness f) {
    return f != DataFreshness::LoadingNoCache && f != DataFreshness::UnavailableNoCache;
}

enum class WriteRoute : std::uint8_t { NetworkFirst, QueueImmediately, Reject };

/// `readOnlyPreference` is the user's Preferences switch — never the connectivity banner.
inline WriteRoute RouteWrite(TrackerConnectivityState s, bool queueSupported, bool readOnlyPreference) {
    if (readOnlyPreference) {
        return WriteRoute::Reject;
    }
    if (IsOfflineState(s) && queueSupported) {
        return WriteRoute::QueueImmediately;
    }
    return WriteRoute::NetworkFirst;
}

} // namespace offline
} // namespace smatchet
