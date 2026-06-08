#pragma once

// GridPane is the persistent, dockable-window unit of the multi-grid foundation
// (ADR-0018, plan multi-grid-tabs Slice 2 / plan items 7 + 10): one grid window's
// identity {id, title, backendKey, viewId} — persisted in smatchet_panes.json via
// ConfigManager::LoadPanesOrBootstrap — plus its per-pane UI runtime (grid edit
// state, sort/filter projection caches, cached ticket snapshot). The runtime block
// was migrated out of the UiDrawSession singleton fields so the grid render is
// re-entrant: SmatchetUI::drawActiveProjectWindow takes a GridPane& and renders
// once per visible pane window per frame, each pane reusing ITS OWN sort/filter
// cache (a steady-state re-render is an O(1) cache hit; a shared/global cache
// would thrash across panes — rejected, see plan § Performance).
// SLICE-2 BOUNDARY (ADR-0018): exactly ONE GridLiveContext is live. The FOCUSED
// pane drives it (sync, edits, catalog); non-focused panes render from their
// cached `ticketsSnapshot` until Slice 3 attaches a live context to every
// visible pane. Dock geometry is NOT stored here — it rides ImGui's .ini.

#include "CachedTicketTypes.h"
#include "SpreadsheetState.h"
#include "TicketGridModel.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GridPane {
    // ---- Identity (persisted, ordered, in smatchet_panes.json) ----
    std::string id;         ///< Stable pane id ("main" for the bootstrap pane).
    std::string title;      ///< Window-tab label (backend tag + view name).
    std::string backendKey; ///< ConfigManager::NormalizeViewsBackendKey output.
    std::string viewId;     ///< Saved-view id inside that backend's views bucket.

    // ---- UI runtime (per-pane, never persisted) ----
    bool focused = false; ///< Mirrors UiDrawSession::focusedPaneId — set by the pane-window host.
    bool open = true;     ///< ImGui close-X writes false; the host enforces the min-1-pane invariant.

    /// Set by the `pane.rename` command: the user pinned a custom tab label, so the
    /// steady-state pane↔active-view lockstep (syncFocusedPaneWithActiveView) must STOP
    /// re-deriving `title` from the active view name. Session-scoped (not serialized to
    /// smatchet_panes.json): the custom label is written to disk via the `title` field
    /// but is reset to the view-derived name on the next launch's first focus sync.
    /// viewId/backendKey still track the live context — only the label is pinned.
    bool titleOverridden = false;

    SpreadsheetState gridState; ///< Active issue, cell edit state, rectangular selection.
    char gridFilterBuf[128] = {};
    char lastFilterBuf[128] = {}; ///< Last filter applied to the cached projection (change detector).

    // Sort + filter projection cache (per-pane so N panes don't thrash one cache).
    std::vector<size_t> cachedSortedIndices;
    std::vector<size_t> filteredIndices;
    std::string cachedSortFingerprint;
    std::uint64_t cachedSortTicketsRevision = 0;
    std::uint64_t cachedSortCatalogRevision = 0;
    bool cachedSortValid = false;
    bool forceApplySortSpecs = false;
    std::chrono::steady_clock::time_point lastGridSortAt{};

    // View-switch / grid-context-change bookkeeping (formerly UiDrawSession fields).
    // NOTE: lastViewsBackendKey stayed SESSION-level (UiDrawSession) — the backend-change
    // session reset guards session-scoped state (catalog, initial sync, live context),
    // so a per-pane delta goes blind on A→B→A pane-focus hops (review HIGH-4).
    std::string lastGridActiveViewId;
    std::string lastGridContextSignature;

    // Wheel-routing hysteresis for the grid table (per-pane — each table scrolls alone).
    int gridBottomHorizontalWheelSwallowsRemaining = 0;
    int gridTopHorizontalWheelSwallowsRemaining = 0;

    /// Ticket snapshot this pane renders. The focused pane refreshes it every frame from
    /// the single live context (cheap shared_ptr copy — no allocation); non-focused panes
    /// keep rendering the cached snapshot (Slice-2 boundary, see header comment).
    std::shared_ptr<const std::vector<CachedTicket>> ticketsSnapshot;
    /// ActiveTickets revision captured when ticketsSnapshot was taken. The sort cache keys
    /// on THIS (not the live revision) so a non-focused pane's frozen snapshot doesn't
    /// re-sort on every background sync tick.
    std::uint64_t snapshotRevision = 0;

    /// Per-pane column-set cache for panes whose viewId differs from the shared per-frame
    /// GridFrameContext (which is built for the focused/active view). Rebuilt only when
    /// catalog/views revisions or the pane's view change — never per frame.
    std::vector<TicketGridColumn> cachedColumns;
    std::uint64_t cachedColumnsCatalogRevision = 0;
    std::uint64_t cachedColumnsViewsRevision = 0;
    std::string cachedColumnsViewId;
    bool cachedColumnsValid = false;

    /// Cached ImGui window name ("<title>###GridPane:<id>") rebuilt only when
    /// title/id change — avoids a per-frame std::string build per pane.
    std::string cachedWindowName;
    std::string cachedWindowNameTitle; ///< Title the cached name was built from.
};

/// Linear pane lookup (pane counts are single-digit; no map needed). Returns
/// nullptr when absent. Inline + pure so bucket-A tests cover it without
/// linking the UI session TU.
inline GridPane* FindGridPaneById(std::vector<GridPane>& panes, const std::string& paneId) {
    for (auto& pane : panes) {
        if (pane.id == paneId) {
            return &pane;
        }
    }
    return nullptr;
}

inline const GridPane* FindGridPaneById(const std::vector<GridPane>& panes, const std::string& paneId) {
    for (const auto& pane : panes) {
        if (pane.id == paneId) {
            return &pane;
        }
    }
    return nullptr;
}
