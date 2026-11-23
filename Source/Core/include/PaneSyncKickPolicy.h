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

/// Re-fetches a pane catalog may make after a dropped apply before the pane is left to its next sync kick.
/// Each one needs a real backend swap or context revival between kick and apply, so the cap only bounds a
/// pane whose backend keeps being recreated.
constexpr unsigned kMaxPaneCatalogRefetches = 2;

/// True when a pane catalog fetched against (`capturedGeneration`, `capturedEpoch`) may still land: the pane's backend
/// was not swapped (GridLiveContext::backendGeneration_) and its catalog was not explicitly cleared, as a tracker
/// switch does first (GridContextFieldCatalog::CatalogEpoch), while the fetch ran.
inline bool PaneCatalogResultCurrent(std::uint64_t capturedGeneration, std::uint64_t currentGeneration,
                                     std::uint64_t capturedEpoch, std::uint64_t currentEpoch) {
    return capturedGeneration == currentGeneration && capturedEpoch == currentEpoch;
}

/// What AppController::applyPaneCatalogOnMainThread_ does with a catalog a worker fetched for a pane.
enum class PaneCatalogApplyVerdict : std::uint8_t {
    Apply,         ///< current, fetched for the pane's site, and the pane has no live catalog: write it in
    DropNotNeeded, ///< nothing is owed: the context is gone or already holds a live catalog for the request's scope
    DropRefetch,   ///< not usable and the pane has no catalog: fetch again with the pane's latest configuration
    DropOwed, ///< not usable, the pane has no catalog, and no re-fetch can run (another site, or the budget is spent)
};

/// Decides a pane-catalog apply (capture-then-check, issue #1081).
/// - `resultCurrent`: PaneCatalogResultCurrent.
/// - `liveCatalogCoversResult`: the context already holds a live catalog (fieldCatalogEverLoaded_) for the scope the
///   fetch was REQUESTED for (never the result's own, which a failed result does not carry): fetched for the same
///   project, or any live catalog when the request is unscoped. Another path (the focused grid fetch, a
///   RefreshFieldCatalog, the same-backend seed, an earlier pane fetch) filled it while this fetch ran, and that
///   catalog is never replaced by this one. A live catalog of another scope does not count: a project-scoped
///   result replaces an unscoped seed.
/// - `resultForPaneSite`: the configuration the fetch ran with names the site the context serves now (its cache key).
///   A result fetched for another site never lands, even when nothing else moved: it would put one tracker's catalog
///   into another's pane.
/// - `latestConfigForPaneSite`: the pane's latest configuration (the last sync kick's) names that site, so a re-fetch
///   with it cannot reach another tracker.
inline PaneCatalogApplyVerdict DecidePaneCatalogApply(bool contextAlive, bool resultCurrent,
                                                      bool liveCatalogCoversResult, bool resultForPaneSite,
                                                      bool latestConfigForPaneSite, unsigned refetchesSoFar) {
    if (!contextAlive || liveCatalogCoversResult) {
        return PaneCatalogApplyVerdict::DropNotNeeded;
    }
    if (resultCurrent && resultForPaneSite) {
        return PaneCatalogApplyVerdict::Apply;
    }
    if (!latestConfigForPaneSite || refetchesSoFar >= kMaxPaneCatalogRefetches) {
        return PaneCatalogApplyVerdict::DropOwed;
    }
    return PaneCatalogApplyVerdict::DropRefetch;
}

/// True when AppController::TickChangeMonitors should dispatch a change-probe for a pane
/// (ticket-change-monitor plan, § Approach #1). Every gate independently blocks: the feature
/// must be enabled, the backend reachable, the app window focused, the pane recently visible
/// (an LRU-evicted pane has an empty ActiveTickets baseline — § Grill outcomes #2 — so it is
/// skipped, never diffed against an empty set), no streaming sync already in flight for the
/// pane, and the per-pane poll interval must have elapsed. Pure so the truth-table is
/// unit-testable without instantiating AppController.
inline bool ShouldPollForChanges(std::chrono::steady_clock::time_point now, bool monitorEnabled, bool backendReachable,
                                 bool windowFocused, bool paneRecentlyVisible, bool syncActive,
                                 std::chrono::steady_clock::time_point nextChangePollAt) {
    return monitorEnabled && backendReachable && windowFocused && paneRecentlyVisible && !syncActive &&
           now >= nextChangePollAt;
}

/// True when a change-probe result captured at (`capturedGeneration`, `capturedEpoch`) may still be
/// applied to its pane. On top of the context/backend checks of `PaneSyncKickStillCurrent`, the
/// probe is dropped when the pane's sync session epoch moved (a streaming sync started after the
/// probe was issued, so the probe's rows are older than what that sync is writing) or a sync is
/// active right now (its batches own the roster until the session ends). The next poll re-checks.
inline bool ChangeProbeStillApplicable(bool contextAlive, std::uint64_t capturedGeneration,
                                       std::uint64_t currentGeneration, std::uint64_t capturedEpoch,
                                       std::uint64_t currentEpoch, bool syncActive) {
    return PaneSyncKickStillCurrent(contextAlive, capturedGeneration, currentGeneration) &&
           capturedEpoch == currentEpoch && !syncActive;
}

} // namespace smatchet
