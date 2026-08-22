#ifndef SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_UI_INTERNAL_H
#define SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_UI_INTERNAL_H

// Private shared header for the SmatchetActiveProjectGridUi partition
// (SmatchetActiveProjectGridUi.cpp = window/orchestrator, SmatchetActiveProjectGridTable.cpp,
// SmatchetActiveProjectGridCells.cpp). Holds ActiveProjectDrawCtx, the per-frame draw context
// threaded by reference through every grid section helper. god-file-splits: byte-identical move
// of the struct out of the monolithic TU. Data-only (references + value snapshots), so every
// member type is forward-declared and the header pulls no heavy deps (notably NOT AppController.h,
// keeping the god-header fan-in flat). Not part of the public API.

#include <cstdint>
#include <vector>

class AppController;
struct UiDrawSession;
struct GridPane;
struct CachedTicket;
class TrackerFieldCatalogIndex;
struct TicketGridColumn;
struct TrackerConnectivityBannerForUi;
struct ViewDefinition;
struct PendingFieldEdit;

// Shared per-frame state for the drawActiveProjectWindow section helpers (monoliths
// Slice 1b). Constructed once at the top of drawActiveProjectWindow from orchestrator-
// owned stack locals; passed by reference into each section helper. Members are
// references / value-snapshots captured ONCE — helpers must not re-fetch them.
struct ActiveProjectDrawCtx {
    AppController& app;
    UiDrawSession& d;
    /// The pane this window renders — resolved ONCE per window (never per cell).
    /// All per-pane grid state (sort/filter caches, selection, filter buffer)
    /// routes through this reference; `pane.focused` gates the session-level
    /// mutation paths (view edits, new-issue draft, modals, toasts).
    GridPane& pane;
    const std::vector<CachedTicket>& tickets;
    const TrackerFieldCatalogIndex& catalogIndex;
    const std::vector<TicketGridColumn>& columns;
    const TrackerConnectivityBannerForUi& trackerBanner;
    ViewDefinition* activeViewForGrid;
    bool readOnlyMode;
    // Embedded (mobile Grid page) draw: the header toolbar suppresses the multi-pane
    // "+" / "▾" controls — mobile is single-pane and its shell never runs
    // ApplyPaneAddAndCloseRequests, so a latched paneAddRequest would go stale there.
    bool embedded;
    // Bound by reference to an orchestrator local assigned after the header toolbar
    // runs, so the view-switch bookkeeping observes any active-view change the toolbar
    // made. The sort helper reads it at table time.
    bool& gridSortEnvironmentChanged;
    // Mutable cross-section grid state (produced inside the table body, consumed by the
    // post-table rect-selection / clear logic). Owned by the orchestrator stack.
    std::vector<PendingFieldEdit>& pendingEdits;
    bool& rectCellClickedThisFrame;
    bool& ticketGridLeftClickInsideTableHit;
    std::uint64_t& gridSortSig;
};

#endif // SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_UI_INTERNAL_H
