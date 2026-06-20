#pragma once

// PaneSyncKickPolicy — pure decision helpers for AppController::EnsurePaneLiveSyncStarted
// (issue #1081). Extracted as header-pure free functions (BackgroundWorkerReap.h precedent) so
// the kick/apply decisions are unit-testable without instantiating the heavy AppController:
//   * ShouldKickInitialSync — the storm-damping gate: a pane whose last streaming session
//     failed gets a 30 s retry window (GridLiveContext::syncRetryAfter) instead of re-kicking
//     a full sync every frame.
//   * PaneSyncKickStillCurrent — the capture-then-check apply gate for the kick's main-thread
//     post: a kick captured against backend generation G must be dropped when the context was
//     retired or its backend swapped (generation moved) while the kick's worker ran —
//     otherwise the queued pre-switch cfgCopy swaps the backend BACK (stale-cfg flip-flop)
//     and seeds the old backend's cache rows into the new backend's grid.

#include <chrono>
#include <cstdint>

namespace smatchet {

/// True when EnsurePaneLiveSyncStarted should kick the pane's initial sync: the one-shot
/// latch is not set AND the failure-backoff retry window (if any) has elapsed.
inline bool ShouldKickInitialSync(std::chrono::steady_clock::time_point now, bool initialSyncKicked,
                                  std::chrono::steady_clock::time_point syncRetryAfter) {
    return !initialSyncKicked && now >= syncRetryAfter;
}

/// True when a kick captured against `capturedGeneration` may still apply: the context is
/// still alive AND its backend generation has not moved since capture.
inline bool PaneSyncKickStillCurrent(bool contextAlive, std::uint64_t capturedGeneration,
                                     std::uint64_t currentGeneration) {
    return contextAlive && capturedGeneration == currentGeneration;
}

/// True when AppController::TickChangeMonitors should dispatch a change-probe for a pane
/// (ticket-change-monitor plan, § Approach #1). Every gate independently blocks: the feature
/// must be enabled, the backend reachable, the app window focused, the pane recently visible
/// (an LRU-evicted pane has an empty ActiveTickets baseline — § Grill outcomes #2 — so it is
/// skipped, never diffed against an empty set), no streaming sync already in flight for the
/// pane, and the per-pane poll interval must have elapsed. Pure so the truth-table is
/// unit-testable without instantiating AppController.
inline bool ShouldPollForChanges(std::chrono::steady_clock::time_point now, bool monitorEnabled,
                                 bool backendReachable, bool windowFocused, bool paneRecentlyVisible,
                                 bool syncActive, std::chrono::steady_clock::time_point nextChangePollAt) {
    return monitorEnabled && backendReachable && windowFocused && paneRecentlyVisible && !syncActive &&
           now >= nextChangePollAt;
}

} // namespace smatchet
