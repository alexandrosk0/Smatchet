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

#include "ConfigManager.h"
#include "GridPane.h"

#include <string>
#include <unordered_map>
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
bool ApplyPaneAddAndCloseRequests(UiDrawSession& d,
                                  const std::unordered_map<std::string, ViewWorkspaceState>& viewBuckets);

// Pure, ImGui-free request-application core (SmatchetGridPaneWindows_detail.cpp)
// so bucket-A tests cover the close/add/focus-reassignment invariants without a
// UI loop (mirrors the SmatchetGridHeaderUi_detail pattern).
namespace detail {

struct PaneRequestApplyOutcome {
    bool Changed = false;         ///< Pane set mutated (close applied or pane added).
    bool FocusReassigned = false; ///< focusedPaneId moved by the host (not by window focus).
};

/// Resolve the view id for a new pane on `backendKey`. Resolution order:
/// requestedViewId (if valid in the bucket) → backend ActiveViewId → first view
/// in bucket → empty string (caller falls back to steady-state view resolve).
/// Pure — takes already-loaded buckets; no disk I/O or ConfigManager calls.
std::string ResolveNewPaneView(const std::string& backendKey, const std::string& requestedViewId,
                               const std::unordered_map<std::string, ViewWorkspaceState>& viewBuckets);

/// The body of ApplyPaneAddAndCloseRequests on bare data: close sweep (min-1
/// invariant, survivor keeps ITS OWN identity — the focus hand-over is reported,
/// never resolved here) + the "+" duplicate request (consumes addRequest.sourceId).
/// viewBuckets is used to resolve the new pane's view when targetBackendKey is
/// non-empty (cross-backend create); pass an empty map when targetBackendKey is
/// always empty (Slice 1 write sites; Slice 2 threads the real buckets through).
PaneRequestApplyOutcome
ApplyPaneAddAndCloseRequestsCore(std::vector<GridPane>& panes, std::string& focusedPaneId, PaneAddRequest& addRequest,
                                 const std::unordered_map<std::string, ViewWorkspaceState>& viewBuckets);

/// True when a dangling pane.viewId may be self-repaired to the active view: only
/// when the pane belongs to the currently-loaded (focused) backend bucket. A
/// cross-backend pane's viewId is valid in its OWN bucket and must never be
/// rebound while another backend is focused (review HIGH-1). Empty pane key =
/// pre-bootstrap placeholder — repair allowed (Pillar 3).
bool PaneViewSelfRepairAllowed(const std::string& paneBackendKey, const std::string& cfgBackendKey);

/// Column-set source for one pane this frame (SmatchetUI::resolvePaneColumns policy core).
enum class PaneColumnsSource {
    SharedActive,  ///< Pane's own view IS the active view — shared per-frame build (captured per pane on change).
    OwnViewBuild,  ///< Own view resolved but not active — per-pane build, rebuilt on revision change only.
    CachedFrozen,  ///< Own view unresolvable (cross-backend bucket not loaded) — keep the bind-time capture.
    SharedFallback ///< No own view AND no capture (cold start / pre-bootstrap) — shared columns.
};

/// Pure column-source policy: only a view that IS the pane's own (strict id match —
/// the same per-pane discipline as the sort-mirror gate) may drive the pane's column
/// set. A fallback-resolved view (the focused backend's active view standing in for a
/// cross-backend pane whose views bucket isn't loaded) must NEVER leak its field set
/// into this pane — user defect: focusing pane B changed unfocused pane A's columns.
PaneColumnsSource ChoosePaneColumnsSource(const std::string& paneViewId, const std::string& resolvedViewId,
                                          const std::string& activeViewId, bool cachedColumnsValid,
                                          const std::string& cachedColumnsViewId);

/// Cold-start upgrade decision (Slice 4): when ChoosePaneColumnsSource yields
/// SharedFallback (no session capture — e.g. a cross-backend pane after restart), the
/// pane context's own resolved view (published by its first-sync worker) can still build
/// the pane's REAL column set, closing the cold-start leak. True only when the source is
/// the fallback AND the context-published view IS the pane's own (strict id match, empty
/// pane id never owns) — so a fallback/foreign view can never drive the build.
bool ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource source, const std::string& paneViewId,
                                           const std::string& ownResolvedViewId);

/// Catalog source for one pane this frame (SmatchetUI::resolvePaneCatalog policy core;
/// per-pane-catalog-value-read-routing). Decides whether a pane resolves its cell option
/// labels / display names against the shared FOCUSED catalog or its OWN context catalog.
enum class PaneCatalogSource {
    SharedFocused, ///< Focused pane OR the pane context has no populated own catalog — use the shared focused index.
    OwnContext ///< Non-focused pane whose OWN context catalog is populated (seed or fetch) — build a per-pane index.
};

/// Pure catalog-source policy. The focused pane always reads the focused catalog (it IS the
/// focused context — no divergence possible). A non-focused pane reads its OWN context
/// catalog only once that context is populated; until then it falls back to the focused
/// catalog, identical to the pre-routing shared read (never a regression — only an upgrade).
/// Deliberately NOT keyed on backend-key equality: a same-backend duplicate pane whose
/// context is seeded (and kept fresh by its own revision) legitimately reads its OWN catalog,
/// and the resolvePaneCatalog cache keys on the per-context revision so it never goes stale.
PaneCatalogSource ChoosePaneCatalogSource(bool paneFocused, bool paneCatalogPopulated);

} // namespace detail

} // namespace SmatchetGridPaneWindows
