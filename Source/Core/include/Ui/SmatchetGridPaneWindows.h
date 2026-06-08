#pragma once

// Pane-window host helpers (multi-grid-tabs Slice 2, plan item 15, ADR-0018).
// The drawing entry point is SmatchetUI::drawGridPaneWindows (declared in
// SmatchetUI.h, defined in SmatchetGridPaneWindows.cpp — it needs SmatchetUI
// members: ViewState, gridFrameCtx_, the re-entrant drawActiveProjectWindow).
// This header exposes the deterministic, ImGui-free pane-management helpers so
// bucket-A tests can cover bootstrap/persist/add/close without a UI loop.
// Slice 3 (plan item 17): EVERY VISIBLE pane is live — drawActiveProjectWindow
// stamps visibility into AppController::EnsurePaneContextLive per pane per frame,
// non-focused visible panes kick their own (config, views) sync, and hidden panes'
// contexts retire after a grace window (AppController::TickAllContexts). The host
// still routes the FOCUSED pane's view/backend swap through the existing sync
// chokepoint (focused-context delegators, ADR-0018).

#include "GridPane.h"

#include <string>
#include <vector>

struct UiDrawSession;

namespace SmatchetGridPaneWindows {

/// One-time bootstrap of d.gridPanes from smatchet_panes.json (via
/// ConfigManager::LoadPanesOrBootstrap). No-op until d.cfgInitialized; idempotent.
void EnsurePanesLoaded(UiDrawSession& d);

/// Serialize d.gridPanes + d.focusedPaneId back to smatchet_panes.json.
void PersistPanes(const UiDrawSession& d);

/// Arm the debounced panes save (mirrors MarkPrefsDirty — ~500 ms window).
void MarkPanesDirty(UiDrawSession& d);

/// Flush the debounced save when due. Called once per frame by the host.
void DrainPanesSaveIfDue(UiDrawSession& d);

/// "pane-N" id not colliding with any existing pane id.
std::string GenerateUniquePaneId(const std::vector<GridPane>& panes);

/// Next pane id after `currentId` in render order, wrapping past the end (multi-grid
/// Slice 4 — drives `pane.next`). Empty panes → empty string; `currentId` not found →
/// the first pane's id (a safe focus fallback). Pure so bucket-A covers the wrap.
std::string NextPaneId(const std::vector<GridPane>& panes, const std::string& currentId);

/// Previous pane id before `currentId` in render order, wrapping past the front
/// (drives `pane.prev`). Same empty / not-found semantics as NextPaneId.
std::string PrevPaneId(const std::vector<GridPane>& panes, const std::string& currentId);

/// Apply the deferred "+" (duplicate source pane) and close-X requests AFTER the
/// window loop — never mutates gridPanes mid-iteration; at least one pane always
/// survives a close sweep. Returns true when the pane set changed. Sets
/// d.gridPaneFocusReassigned when it moved focusedPaneId (review HIGH-2: the host
/// must replay the reassignment as a real focus switch next frame so the new
/// focused pane's saved view gets activated).
bool ApplyPaneAddAndCloseRequests(UiDrawSession& d);

// Pure, ImGui-free request-application core (SmatchetGridPaneWindows_detail.cpp)
// so bucket-A tests cover the close/add/focus-reassignment invariants without a
// UI loop (mirrors the SmatchetGridHeaderUi_detail pattern).
namespace detail {

struct PaneRequestApplyOutcome {
    bool Changed = false;         ///< Pane set mutated (close applied or pane added).
    bool FocusReassigned = false; ///< focusedPaneId moved by the host (not by window focus).
};

/// The body of ApplyPaneAddAndCloseRequests on bare data: close sweep (min-1
/// invariant, survivor keeps ITS OWN identity — the focus hand-over is reported,
/// never resolved here) + the "+" duplicate request (consumes addRequestSourceId).
PaneRequestApplyOutcome ApplyPaneAddAndCloseRequestsCore(std::vector<GridPane>& panes, std::string& focusedPaneId,
                                                         std::string& addRequestSourceId);

/// True when a dangling pane.viewId may be self-repaired to the active view: only
/// when the pane belongs to the currently-loaded (focused) backend bucket. A
/// cross-backend pane's viewId is valid in its OWN bucket and must never be
/// rebound while another backend is focused (review HIGH-1). Empty pane key =
/// pre-bootstrap placeholder — repair allowed (Pillar 3).
bool PaneViewSelfRepairAllowed(const std::string& paneBackendKey, const std::string& cfgBackendKey);

} // namespace detail

} // namespace SmatchetGridPaneWindows
