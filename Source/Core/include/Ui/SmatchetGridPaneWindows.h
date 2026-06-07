#pragma once

// Pane-window host helpers (multi-grid-tabs Slice 2, plan item 15, ADR-0018).
// The drawing entry point is SmatchetUI::drawGridPaneWindows (declared in
// SmatchetUI.h, defined in SmatchetGridPaneWindows.cpp — it needs SmatchetUI
// members: ViewState, gridFrameCtx_, the re-entrant drawActiveProjectWindow).
// This header exposes the deterministic, ImGui-free pane-management helpers so
// bucket-A tests can cover bootstrap/persist/add/close without a UI loop.
//
// SLICE-2 BOUNDARY (keep until Slice 3): ONE GridLiveContext is live. The
// focused pane drives it; the host only switches WHICH pane is focused (and
// routes the per-pane view/backend swap through the existing sync chokepoint).
// No multi-context lifecycle lives here.

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

/// Apply the deferred "+" (duplicate source pane) and close-X requests AFTER the
/// window loop — never mutates gridPanes mid-iteration; at least one pane always
/// survives a close sweep. Returns true when the pane set changed.
bool ApplyPaneAddAndCloseRequests(UiDrawSession& d);

} // namespace SmatchetGridPaneWindows
