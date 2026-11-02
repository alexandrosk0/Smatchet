#pragma once

// ConnectivityProbeSchedule — pure scheduling helpers for ConnectivityMonitorService's tracker
// reachability probe, split out so the backoff + live-success push are unit-testable without
// driving the async probe FSM.
// Plan: docs/plans/background-traffic-idle-gating.md

#include <algorithm>
#include <chrono>

namespace smatchet {

/// Healthy steady state: one probe per this interval when no other traffic proved reachability.
constexpr std::chrono::seconds kProbeRelaxedInterval{90};
/// First retry after a degraded probe; doubles per consecutive degraded result up to the cap.
constexpr std::chrono::seconds kProbeAggressiveInterval{20};
constexpr std::chrono::seconds kProbeDegradedBackoffCap{120};

/// Delay before the next probe after `consecutiveDegraded` degraded results in a row (1-based):
/// 20 s, 40 s, 80 s, then 120 s. Values below 1 are treated as 1.
inline std::chrono::seconds DegradedProbeInterval(int consecutiveDegraded) {
    std::chrono::seconds interval = kProbeAggressiveInterval;
    for (int i = 1; i < consecutiveDegraded && interval < kProbeDegradedBackoffCap; ++i) {
        interval *= 2;
    }
    return (std::min)(interval, kProbeDegradedBackoffCap);
}

/// Next probe deadline after a successful live backend request. A request that succeeded while the
/// FSM was already healthy proves reachability, so the probe moves out a full relaxed interval. A
/// degraded prior state, or a probe already in flight, leaves the deadline alone so the recovery
/// path keeps its existing timing.
inline std::chrono::steady_clock::time_point
NextProbeAfterLiveSuccess(std::chrono::steady_clock::time_point nextProbeAt, std::chrono::steady_clock::time_point now,
                          bool wasHealthy, bool probeInFlight) {
    if (!wasHealthy || probeInFlight) {
        return nextProbeAt;
    }
    return (std::max)(nextProbeAt, now + kProbeRelaxedInterval);
}

} // namespace smatchet
