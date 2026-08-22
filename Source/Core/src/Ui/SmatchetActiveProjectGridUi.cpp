#include "SmatchetUI.h"
#include "SmatchetActiveProjectGridUi_Internal.h"
#include "SmatchetGridPaneWindows.h" // detail::PaneViewSelfRepairAllowed (HIGH-1) + ChoosePaneColumnsSource
#include "SmatchetGridUiSupport.h"
#include "SmatchetDockNodeIds.h" // central-node re-dock for extra panes on a layout reset
#include "SmatchetViewsDashboardUi_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "SmatchetCommentsModalUi.h"
#include "SmatchetFieldRender.h"
#include "SmatchetInputModifierBridge.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"
#include "Ui/SmatchetTooltipWheelRouter.h"
#include "UiPerfMonitor.h"

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared SmatchetActiveProjectGrid-TU include prologue is grandfathered across the god-file-split siblings (SmatchetActiveProjectGridUi.cpp / _Table / _Cells) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared Grid TU prologue header is introduced)
// clang-format on
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui
#include <ghc/filesystem.hpp>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

// Pre-Begin placement for an EXTRA (non-bootstrap) pane. Extra panes deliberately do not
// share the bootstrap pane's "active" layout key: d.pendingReDockWindows is keyed by that
// string, so the first pane in the loop consumes the arm and a later floating pane re-inserts
// it — N windows fighting over one latch written for exactly one window. The static "active"
// dock slot would also scatter every new pane into the default grid node instead of the tab
// bar whose "+" the user actually clicked. Placement comes from the source pane's live node
// instead (seeded at creation by ApplyPaneAddAndCloseRequestsCore).
void PrepareExtraPaneWindow(UiDrawSession& d, GridPane& pane, bool wantFocus) {
    // Layout reset (#2082): an extra pane's window name carries a "###GridPane:<id>" settings
    // id the default-layout ini has no entry for, so LoadIniSettingsFromMemory clears its
    // DockId and NOTHING re-docks it — pendingReDockWindows is keyed by the layout-key table,
    // which extra panes deliberately stay out of. Every extra pane was therefore left floating
    // and then snapped by repairTopLevelWindow's forced-defaults branch to the same centred
    // fallback rect, i.e. "all but one pane undocked in the middle of the screen". The policy
    // (reset → central node, else the one-shot "+" hand-off) is the pure core below.
    const bool layoutResetSettling = d.layoutForceDefaultsFrames > 0;
    const ImGuiID liveCentral =
        layoutResetSettling ? SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::kCentralNode) : 0;
    const unsigned int forcedDock = SmatchetGridPaneWindows::detail::ResolveExtraPaneDockTarget(
        pane.pendingDockId, static_cast<unsigned int>(liveCentral), layoutResetSettling);
    if (forcedDock != 0) {
        ImGui::SetNextWindowDockID(static_cast<ImGuiID>(forcedDock), ImGuiCond_Always);
    }
    ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
}

// ExtraPaneRectRepairSkippedDuringLayoutReset — why drawActiveProjectWindow skips an extra
// pane's repairTopLevelWindow call while d.layoutForceDefaultsFrames > 0. That path FORCES the
// fallback rect, and for an unregistered layout key ("grid-pane") the fallback IS the centred
// one: it is what piled every extra pane in the middle of the screen (#2082). It would also
// fire on exactly the frames a pane is legitimately still floating, waiting for the rebuilt
// central node to come alive so the re-dock above can land. Normal off-screen / degenerate-rect
// rescue resumes the frame the countdown expires.

} // namespace

// Re-entrant per-pane grid window (multi-grid-tabs Slice 2, plan item 14). Called once
// per visible GridPane per frame by drawGridPaneWindows (SmatchetGridPaneWindows.cpp),
// which owns pane bootstrap / focus tracking / the min-1-pane close invariant and sets
// d.activePaneForDraw around this call. Slice 3: EVERY visible pane is live — each
// tracks its own GridLiveContext's published snapshot (the focused pane through the
// focused-context delegators); session-level mutation paths (view edits, new-issue
// draft, modals, toasts) stay gated on pane.focused.
void SmatchetUI::drawActiveProjectWindow(AppController& app, UiDrawSession& d, GridPane& pane,
                                         const TrackerConnectivityBannerForUi& TrackerBanner, bool embedded) {
    const bool wantFocus = pane.focused && d.requestActiveProjectFocus;
    // embedded (dual-ui slice 4): the mobile Grid page draws the focused pane's body
    // directly into the page child; skip the dock-window chrome (prepareTopLevelWindow/
    // Begin/End/focus-report) and the multi-pane "+" strip (mobile is single-pane). The
    // desktop path below is byte-identical to the pre-slice-4 flow.
    const bool isBootstrapPane = (pane.id == "main");
    if (!embedded) {
        if (isBootstrapPane) {
            prepareTopLevelWindow(d, "active", 900.0f, 620.0f, wantFocus);
        } else {
            PrepareExtraPaneWindow(d, pane, wantFocus);
        }
        // The bootstrap pane keeps the legacy window name so existing imgui.ini dock
        // geometry and the default-layout ini keep applying; extra panes carry their
        // title with a stable ###GridPane:<id> settings id (cached — no per-frame build).
        if (pane.cachedWindowName.empty() || pane.cachedWindowNameTitle != pane.title) {
            pane.cachedWindowNameTitle = pane.title;
            pane.cachedWindowName =
                (pane.id == "main") ? std::string("Smatchet - Active Project") : pane.title + "###GridPane:" + pane.id;
        }
        // Close button only when another pane remains (min-1-pane invariant; the host
        // applies the actual close after the pane loop).
        bool* paneOpen = (d.gridPanes.size() > 1) ? &pane.open : nullptr;
        ImGuiWindowFlags paneFlags = ImGuiWindowFlags_NoCollapse;
        // The main pane normally hides its title bar (docked, it shows only its tab).
        // Expanded it is floating, and a floating window with no title bar has nowhere
        // to put the minimize half of the toggle — so the flag lifts for those frames.
        if (isBootstrapPane && !SmatchetWindowExpand::IsWindowExpanded(d, pane.cachedWindowName.c_str())) {
            paneFlags |= ImGuiWindowFlags_NoTitleBar;
        }
        SmatchetWindowExpand::BeginWindow(d, pane.cachedWindowName.c_str());
        const bool paneVisible = ImGui::Begin(pane.cachedWindowName.c_str(), paneOpen, paneFlags);
        // Record where this pane ended up EVERY frame, including the clipped/unselected-tab
        // frames that return false — a pane sitting behind its sibling's tab is still docked,
        // and it is exactly the pane a later "+" needs to hand its node to.
        pane.lastDockId = static_cast<unsigned int>(ImGui::GetWindowDockID());
        // Fire the tab-select arm BEFORE the visibility early-return. A pane that docked
        // behind its sibling is exactly the case this fixes, and Begin returns false for it —
        // gating on paneVisible would leave the arm set forever on the one frame it matters.
        if (pane.selectTabFrames > 0) {
            --pane.selectTabFrames;
            selectCurrentDockedTab();
        }
        if (!paneVisible) {
            ImGui::End();
            return;
        }
        SmatchetWindowExpand::DrawToggle(d);
        if (isBootstrapPane) {
            repairTopLevelWindow(d, "active", 420.0f, 300.0f);
        } else if (d.layoutForceDefaultsFrames <= 0) {
            // Rect-repair-only (unregistered key = no dock slot), and skipped while a layout
            // reset settles — see ExtraPaneRectRepairSkippedDuringLayoutReset above for why.
            repairTopLevelWindow(d, "grid-pane", 420.0f, 300.0f);
        }
        if (wantFocus) {
            ImGui::SetWindowFocus();
            d.requestActiveProjectFocus = false;
        }
        // Report window focus to the pane host (consumed after the pane loop). Focus is
        // what flips which pane drives the single Slice-2 live context.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            d.paneWindowFocusedThisFrame = pane.id;
        }
    }
    // Banner resolved once per frame by the pane-window host and passed in.
    if (pane.focused) {
        MaybeToastTrackerConnectivityBanner(app, d, TrackerBanner);
    }
    const bool readOnlyMode =
        d.cfg.ReadOnlyMode || (TrackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error);

    // Visibility-driven context lifecycle (multi-grid Slice 3, plan item 17): this code
    // runs only when ImGui::Begin returned true — i.e. the pane is VISIBLE (a docked-behind
    // tab returns false and skips it, so its lastVisibleAt stops advancing and the context
    // retires after the grace window in TickAllContexts). EnsurePaneContextLive is an O(map
    // lookup) stamp per visible pane per frame — not per cell.
    app.EnsurePaneContextLive(pane.id, pane.backendKey);
    TrackerConfig paneCfg = d.cfg;
    if (!pane.backendKey.empty()) {
        paneCfg.TrackerType = pane.backendKey;
    }
    if (!pane.focused) {
        // Non-focused visible pane is LIVE too: kick its first sync against its own
        // (config, views) pair (one-shot per context generation; views-bucket load runs on
        // a worker — Pillar 2). The focused pane uses the same kick when not sync-live yet.
        app.EnsurePaneLiveSyncStarted(pane.id, paneCfg, pane.viewId);
    } else if (!app.IsPaneSyncLive(pane.id)) {
        app.EnsurePaneLiveSyncStarted(pane.id, paneCfg, pane.viewId);
        if (!d.initialTicketSyncStarted) {
            d.initialTicketSyncStarted = true;
            d.appliedInitialView = true;
        }
    }

    // Snapshot policy (Slice 3): the focused pane tracks the focused live context every
    // frame (cheap shared_ptr copy); a non-focused VISIBLE pane tracks its OWN context's
    // published snapshot, falling back to its cached snapshot until the first sync lands.
    if (pane.focused) {
        pane.ticketsSnapshot = app.GetActiveTicketsSnapshot();
        pane.snapshotRevision = app.GetActiveTicketsRevision();
    } else {
        std::shared_ptr<const std::vector<CachedTicket>> live = app.GetPaneTicketsSnapshot(pane.id);
        if (live) {
            pane.ticketsSnapshot = live;
            pane.snapshotRevision = app.GetPaneTicketsRevision(pane.id);
        } else if (!pane.ticketsSnapshot) {
            pane.ticketsSnapshot = app.GetActiveTicketsSnapshot();
            pane.snapshotRevision = app.GetActiveTicketsRevision();
        }
    }
    const auto ticketsSnap = pane.ticketsSnapshot; // keep alive across the draw
    const auto& tickets = *ticketsSnap;

    ViewDefinition* activeViewForGrid = resolvePaneView(d, pane);
    // Per-pane catalog READ routing + column resolution in one helper (shared with the mobile
    // shell): routes the pane's cell option labels / display names through its OWN context
    // catalog when populated, and keys the column cache on the routed revision. See
    // resolvePaneCatalogAndColumns_.
    const TrackerFieldCatalogIndex* catalogIndexPtr = nullptr;
    const std::vector<TicketGridColumn>& columns =
        resolvePaneCatalogAndColumns_(app, pane, activeViewForGrid, &catalogIndexPtr);
    const TrackerFieldCatalogIndex& catalogIndex = *catalogIndexPtr;

    // Orchestrator-owned cross-section state (was function-local in the monolith). These
    // outlive every section helper and are bound by reference into the DrawCtx below.
    std::vector<PendingFieldEdit> pendingEdits;
    bool rectCellClickedThisFrame = false;
    bool ticketGridLeftClickInsideTableHit = false;
    std::uint64_t gridSortSig = 0;
    bool gridSortEnvironmentChanged = false;

    ActiveProjectDrawCtx ctx{app,
                             d,
                             pane,
                             tickets,
                             catalogIndex,
                             columns,
                             TrackerBanner,
                             activeViewForGrid,
                             readOnlyMode,
                             gridSortEnvironmentChanged,
                             pendingEdits,
                             rectCellClickedThisFrame,
                             ticketGridLeftClickInsideTableHit,
                             gridSortSig};

    drawActiveProjectHeader(ctx);

    applyActiveProjectViewChange(ctx);

    // Unsaved-layout strip + offline-queues panel (the unscoped "above the table"
    // header UI). All BeginChild/EndChild + BeginPopupModal/EndPopup pairs stay inside
    // the helper — never split across this boundary. Focused pane only (session-level
    // view-edit state + global offline queues).
    if (pane.focused) {
        drawActiveProjectUnsavedStrip(ctx);
    }

    drawActiveProjectTable(ctx);

    // Google-Sheets-style selection: end drag on mouse release, clear on outside
    // click, and service Ctrl+C (copy as TSV) / Escape (clear) / Shift+Space (whole
    // row) while focused. The helper early-returns when no grid was drawn this frame.
    drawActiveProjectGridRectSelKeys(ctx);

    // Long-text / ADF modal editor lives at top-level so it survives the originating cell scrolling out
    // of view. Edits accepted in the modal are appended to `pendingEdits` and flow through the same
    // EnqueueGridFieldEdits path below (host pumps once per frame). The popup stack is global —
    // render once, from the focused pane.
    if (pane.focused) {
        TicketFieldEditor::RenderLongTextModal(pendingEdits);
        // issue-comments PR-A — backend-agnostic comments read/post modal. Top-level so it survives
        // the originating cell scrolling out of view; the popup stack is global, so render once from
        // the focused pane. readOnlyMode disables the post box.
        RenderCommentsModal(app, readOnlyMode);
    }

    // Enqueue only — the pane-window host runs the dispatch pump + chip decay ONCE
    // per frame against the focused pane's live snapshot (review MEDIUM-1: a per-pane
    // pump faded chips N× faster and could snapshot estimate bases from a non-focused
    // pane's frozen snapshot).
    EnqueueGridFieldEdits(d, pendingEdits, readOnlyMode);
    if (pane.focused) {
        MaybeToastGridBannerFromSession(d);
    }
    if (!embedded) {
        ImGui::End();
    }
}

// Resolve the pane's view inside the (focused-backend) views bucket. A SAME-backend
// pane referencing a deleted/unknown view falls back to the ACTIVE view and
// self-repairs its viewId (Pillar 3 — never dangle). A CROSS-backend pane's viewId
// lives in ITS OWN backend's bucket — the loaded slice simply can't see it — so it
// returns the active-view FALLBACK without any identity write (review HIGH-1: the
// unconditional repair rebound such panes to the focused view every frame, and the
// debounced PersistPanes made the loss durable). Callers must treat the fallback as
// NOT the pane's own view (strict id-vs-pane.viewId match): the sort mirror refuses
// to write through it, and resolvePaneColumns refuses to render its field set
// (per-pane column isolation — the frozen bind-time capture wins). Returns null only
// when the loaded bucket has no views at all.
ViewDefinition* SmatchetUI::resolvePaneView(UiDrawSession& d, GridPane& pane) {
    ViewsStore& store = ViewState.GetStoreMutable();
    for (auto& view : store.Views) {
        if (view.Id == pane.viewId) {
            return &view;
        }
    }
    ViewDefinition* active = ViewState.GetActiveViewMutable();
    if (SmatchetGridPaneWindows::detail::PaneViewSelfRepairAllowed(
            pane.backendKey, ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType))) {
        pane.viewId = active ? active->Id : std::string();
    }
    return active;
}

// Catalog INDEX a pane resolves its cell option labels / display names through
// (per-pane-catalog-value-read-routing). Source policy is the pure ChoosePaneCatalogSource
// core: the focused pane (and any pane whose own context catalog isn't populated yet) uses
// the shared per-frame focused index; a non-focused pane whose OWN context catalog is
// populated (same-backend seed OR the per-pane fetch in populatePaneCatalogAfterSync_) gets
// a per-pane index. That index is built ONCE and rebuilt only when the pane's own context
// catalog REVISION changes — so a same-backend duplicate pane re-seeded by a focused refetch
// picks up the fresh catalog (staleness invalidation) and a cross-backend pane picks up its
// own fetched catalog, all without a per-cell rebuild (Pillar 1).
const TrackerFieldCatalogIndex& SmatchetUI::resolvePaneCatalog(AppController& app, GridPane& pane,
                                                               const TrackerFieldCatalogIndex& sharedFocusedIndex) {
    typedef SmatchetGridPaneWindows::detail::PaneCatalogSource PaneCatalogSource;
    const bool populated = !pane.focused && app.IsPaneFieldCatalogPopulated(pane.id);
    const PaneCatalogSource source = SmatchetGridPaneWindows::detail::ChoosePaneCatalogSource(pane.focused, populated);
    if (source == PaneCatalogSource::SharedFocused) {
        // Focused / unpopulated pane: the shared per-frame focused index is exactly the data
        // the pre-routing shared read used — never worse. Keep the per-pane cache marked stale
        // so a later focus flip back to OwnContext rebuilds against the current revision.
        pane.cachedPaneCatalogValid = false;
        return sharedFocusedIndex;
    }
    // OwnContext: build/refresh the per-pane index only when this context's catalog revision
    // moved (or the cache is empty). O(map lookup) + a bounded field-list copy on CHANGE only —
    // steady-state re-renders hit the cached index with zero allocation, so never per cell.
    const std::uint64_t rev = app.GetPaneFieldCatalogRevision(pane.id);
    if (!pane.cachedPaneCatalogValid || !pane.cachedPaneCatalogIndex || pane.cachedPaneCatalogRevision != rev) {
        // Own the fields COPY so the index's raw TrackerField* stay valid (the index points
        // INTO this vector — GetPaneAvailableFields returns a value, not a live reference).
        pane.cachedPaneCatalogFields =
            std::make_shared<const std::vector<TrackerField>>(app.GetPaneAvailableFields(pane.id));
        pane.cachedPaneCatalogIndex = std::make_shared<const TrackerFieldCatalogIndex>(*pane.cachedPaneCatalogFields);
        pane.cachedPaneCatalogRevision = rev;
        pane.cachedPaneCatalogValid = true;
    }
    return *pane.cachedPaneCatalogIndex;
}

// Combined per-pane catalog route + column resolve (per-pane-catalog-value-read-routing).
// Shared by the desktop grid and the mobile shell (DRY Pillar 5) so the routing + effective-
// revision + own-view-fetch + column-cache sequence lives in ONE place. Returns the resolved
// column set; when outCatalogIndex is non-null, also hands back the routed catalog index (the
// desktop cell path needs it — mobile passes null). Resolved ONCE per pane per frame.
const std::vector<TicketGridColumn>&
SmatchetUI::resolvePaneCatalogAndColumns_(AppController& app, GridPane& pane, ViewDefinition* activeView,
                                          const TrackerFieldCatalogIndex** outCatalogIndex) {
    const TrackerFieldCatalogIndex& catalogIndex = resolvePaneCatalog(app, pane, *gridFrameCtx_.catalogIndex);
    if (outCatalogIndex != nullptr) {
        *outCatalogIndex = &catalogIndex;
    }
    // Effective catalog revision: the OWN context revision when the pane routed to its own
    // catalog, else the focused revision — keys the column cache so a per-pane catalog refetch
    // invalidates the header labels (which come from catalog.DisplayName). Equal to the focused
    // revision for a shared-index pane, so the pre-routing behaviour is preserved.
    const std::uint64_t effectiveCatalogRevision =
        pane.cachedPaneCatalogValid ? pane.cachedPaneCatalogRevision : gridFrameCtx_.catalogRevision;
    // Cross-backend cold-start defense (Slice 4): the pane's OWN view resolved by its context
    // sync lets resolvePaneColumns build the pane's OWN column SET even when the focused
    // ViewState bucket can't see it and no session capture exists yet (after a restart).
    std::shared_ptr<const ViewDefinition> paneOwnResolvedView =
        pane.focused ? nullptr : app.GetPaneResolvedView(pane.id);
    return resolvePaneColumns(pane, catalogIndex, activeView, paneOwnResolvedView.get(), effectiveCatalogRevision);
}

// Column set for a pane (per-pane column isolation — the unfocused pane must keep ITS
// OWN view's field set across focus switches). Source policy is the pure
// ChoosePaneColumnsSource core (bucket-A covered): the shared per-frame GridFrameContext
// only when the pane's OWN view (strict id match — same discipline as the sort-mirror
// gate) is the active one; an own-but-inactive view uses the per-pane cached build; a
// fallback-resolved view (cross-backend pane whose views bucket isn't loaded) renders
// the FROZEN bind-time capture instead of leaking the focused view's columns (user
// defect: focusing pane B changed unfocused pane A's columns). The capture below is
// keyed on catalog/views revisions + viewId — refreshed on change only, never per frame.
const std::vector<TicketGridColumn>& SmatchetUI::resolvePaneColumns(GridPane& pane,
                                                                    const TrackerFieldCatalogIndex& catalogIndex,
                                                                    const ViewDefinition* paneView,
                                                                    const ViewDefinition* paneOwnResolvedView,
                                                                    std::uint64_t effectiveCatalogRevision) {
    // effectiveCatalogRevision is the revision of the catalog `catalogIndex` was resolved from
    // (per-pane-catalog-value-read-routing): the pane's OWN context revision when it routed to
    // its own catalog, else the focused revision. Keying the column cache on it invalidates the
    // header labels (built from catalog.DisplayName) when the routed catalog changes — for a
    // shared-index pane it equals gridFrameCtx_.catalogRevision, so behaviour is preserved.
    typedef SmatchetGridPaneWindows::detail::PaneColumnsSource PaneColumnsSource;
    const PaneColumnsSource source = SmatchetGridPaneWindows::detail::ChoosePaneColumnsSource(
        pane.viewId, paneView ? paneView->Id : std::string(), gridFrameCtx_.activeViewId, pane.cachedColumnsValid,
        pane.cachedColumnsViewId);
    // Cold-start frozen-capture hole (Slice 4): a cross-backend pane with no session capture
    // would render the focused view's column set (SharedFallback). Its OWN view, resolved by
    // its context sync (paneOwnResolvedView, matched on viewId), builds the pane's real
    // columns here through the pane's OWN catalog (catalogIndex is already pane-routed). The
    // result is captured below so subsequent frames hit CachedFrozen — never per cell.
    if (paneOwnResolvedView != nullptr && SmatchetGridPaneWindows::detail::ShouldBuildColumnsFromOwnResolvedView(
                                              source, pane.viewId, paneOwnResolvedView->Id)) {
        if (!pane.cachedColumnsValid || pane.cachedColumnsViewId != paneOwnResolvedView->Id ||
            pane.cachedColumnsCatalogRevision != effectiveCatalogRevision ||
            pane.cachedColumnsViewsRevision != gridFrameCtx_.viewsRevision) {
            pane.cachedColumns = TicketGridColumnsBuilder::Build(*paneOwnResolvedView, catalogIndex);
            pane.cachedColumnsCatalogRevision = effectiveCatalogRevision;
            pane.cachedColumnsViewsRevision = gridFrameCtx_.viewsRevision;
            pane.cachedColumnsViewId = paneOwnResolvedView->Id;
            pane.cachedColumnsValid = true;
        }
        return pane.cachedColumns;
    }
    if (source == PaneColumnsSource::SharedFallback) {
        return gridFrameCtx_.columns;
    }
    if (source == PaneColumnsSource::CachedFrozen) {
        return pane.cachedColumns;
    }
    // Own view resolved (SharedActive / OwnViewBuild): keep the per-pane capture fresh.
    // SharedActive copies the shared build (no second TicketGridColumnsBuilder run) so a
    // later cross-backend focus switch still finds this pane's own column set cached.
    if (!pane.cachedColumnsValid || pane.cachedColumnsCatalogRevision != effectiveCatalogRevision ||
        pane.cachedColumnsViewsRevision != gridFrameCtx_.viewsRevision || pane.cachedColumnsViewId != paneView->Id) {
        pane.cachedColumns = (source == PaneColumnsSource::SharedActive)
                                 ? gridFrameCtx_.columns
                                 : TicketGridColumnsBuilder::Build(*paneView, catalogIndex);
        pane.cachedColumnsCatalogRevision = effectiveCatalogRevision;
        pane.cachedColumnsViewsRevision = gridFrameCtx_.viewsRevision;
        pane.cachedColumnsViewId = paneView->Id;
        pane.cachedColumnsValid = true;
    }
    return (source == PaneColumnsSource::SharedActive) ? gridFrameCtx_.columns : pane.cachedColumns;
}

// View-switch + grid-context-change bookkeeping. Split out of drawActiveProjectWindow under the
// function-size cap; behaviour-identical (the body moved verbatim, writing gridSortEnvironmentChanged
// back through the ctx reference instead of the former orchestrator local).
void SmatchetUI::applyActiveProjectViewChange(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    GridPane& pane = ctx.pane;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;

    // Sort-read ownership gate (same class as the columns fix):
    // for a pane whose resolved view is the FALLBACK (cross-backend unfocused pane —
    // resolvePaneView returned the other backend's active view), tracking that fallback
    // id in lastGridActiveViewId flipped viewChanged on every focus switch, which reset
    // the pane's sort and applied the FOCUSED view's SortSpecs onto it. A non-owned
    // resolution leaves the pane's view-change tracking and sort environment untouched.
    const bool viewIsPanesOwn = activeViewForGrid && activeViewForGrid->Id == pane.viewId;
    const bool viewChanged = viewIsPanesOwn && (activeViewForGrid->Id != pane.lastGridActiveViewId);
    if (viewChanged) {
        pane.lastGridActiveViewId = activeViewForGrid->Id;
        // Session-level unsaved-edit state belongs to the focused pane's view only.
        if (pane.focused) {
            d.viewSortDirty = false;
            // Active view switched — abandon unsaved edits that belonged to the old
            // view (no confirmation in the grid path; Views editor has its own modal).
            d.viewsDirty = false;
            d.viewsHasOriginalSnapshot = false;
        }
    }
    if (!activeViewForGrid) {
        pane.lastGridActiveViewId.clear();
        if (pane.focused) {
            d.viewSortDirty = false;
            d.viewsDirty = false;
            d.viewsHasOriginalSnapshot = false;
        }
    }

    // Signature keyed on the pane's OWN identity (review HIGH-2 second part): using the
    // session JQL (d.cfg.JqlQuery) for every pane meant each focus switch rewrote every
    // pane's signature → cachedSortValid invalidated across all panes (resort cascade).
    // Focused pane: session JQL (tracks ad-hoc JQL edits, the original behaviour).
    // Unfocused owned pane: the view's saved JQL (its query of record — stable across
    // focus switches). Non-owned (fallback) resolution: freeze on the pane's stored
    // identity so the signature can't follow the other backend's active view.
    std::string gridContextSignature;
    if (viewIsPanesOwn) {
        gridContextSignature =
            BuildGridContextSignature(activeViewForGrid, pane.focused ? d.cfg.JqlQuery : activeViewForGrid->Jql);
    } else {
        gridContextSignature = std::string("frozen\x1e") + pane.viewId;
    }
    const bool gridContextChanged =
        !pane.lastGridContextSignature.empty() && gridContextSignature != pane.lastGridContextSignature;
    if (gridContextChanged && pane.focused) {
        CancelUnfinishedNewIssueForGridChange(d);
    }
    pane.lastGridContextSignature = gridContextSignature;

    ctx.gridSortEnvironmentChanged = viewChanged || gridContextChanged;
}

// Section helpers for drawActiveProjectWindow (monoliths Slice 1b). Each owns one
// pre-existing SMATCHET_UI_PERF_SCOPE seam verbatim. Bodies are byte-for-byte copies of
// the former inline blocks; the local-alias preamble re-binds the orchestrator-owned ctx
// members to the bare names the moved code already used, leaving the logic untouched.
void SmatchetUI::drawActiveProjectHeader(ActiveProjectDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;
    const bool readOnlyMode = ctx.readOnlyMode;
    const TrackerConnectivityBannerForUi& TrackerBanner = ctx.trackerBanner;

    SMATCHET_UI_PERF_SCOPE("activeProject:header");
    DrawGridHeaderToolbar(app, d, activeViewForGrid, columns, tickets, readOnlyMode, ViewState, TrackerBanner);
}

void SmatchetUI::drawActiveProjectUnsavedStrip(ActiveProjectDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;

    // -------- Unsaved layout strip --------
    // Appears whenever d.viewsDirty OR d.viewSortDirty is true — fed by grid column
    // reorder, sort changes, OR buffer edits made in the Views editor window. Lets
    // the user commit, discard, or fork the in-memory edits without leaving the grid.
    // The frame fence lets a screenshot scenario hide the strip for its capture window
    // without clearing the dirty flags (see UiDrawSession::suppressUnsavedLayoutStripUntilFrame).
    const bool stripFenced = ImGui::GetFrameCount() <= d.suppressUnsavedLayoutStripUntilFrame;
    if ((d.viewsDirty || d.viewSortDirty) && activeViewForGrid && !stripFenced) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.32f, 0.27f, 0.10f, 0.35f));
        ImGui::BeginChild("##UnsavedLayoutStrip", ImVec2(0, ImGui::GetFrameHeightWithSpacing() + 4.0f), true,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "●");
        ImGui::SameLine();
        ImGui::Text("Unsaved layout changes to \"%s\"", activeViewForGrid->Name.c_str());
        ImGui::SameLine();
        const float saveW = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        const float saveAsW = ImGui::CalcTextSize("Save as new...").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        const float discardW = ImGui::CalcTextSize("Discard").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float cluster = saveW + saveAsW + discardW + spacing * 2.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > cluster) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - cluster));
        }
        if (ImGui::Button("Save")) {
            // Commit editing buffers + currently-stored widths/sort onto the active view.
            ViewDefinition updated = *activeViewForGrid;
            updated.Name = d.viewNameBuf[0] ? std::string(d.viewNameBuf) : activeViewForGrid->Name;
            updated.Jql = d.viewJqlEditor.buf[0] ? std::string(d.viewJqlEditor.buf) : activeViewForGrid->Jql;
            // Authoritative selection set, not the truncating buffer (#views-field-uncheck) — a
            // large selection persists in full instead of being clipped on disk at the 1023-byte cap.
            const std::vector<std::string> editedFields =
                SmatchetViewsDashboardUiDetail::ToSortedVector(d.selectedFieldSet);
            if (!editedFields.empty()) {
                updated.Fields = editedFields;
            }
            if (!d.editingColumnOrder.empty()) {
                updated.ColumnOrder = d.editingColumnOrder;
            }
            if (ViewState.UpdateActive(updated)) {
                d.cfg.JqlQuery = updated.Jql;
                d.cfg.SelectedFields = updated.Fields;
                d.lastSyncedColumnOrder = updated.ColumnOrder;
                d.viewsDirty = false;
                d.viewSortDirty = false;
                d.viewsHasOriginalSnapshot = false;
                d.pendingViewStateSave = false;
                ConfigManager::Save(d.cfg);
                ViewState.Save();
                SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.view_saved", "View saved"),
                                                      updated.Name, ToastType::Success, 1500);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as new...")) {
            activeProjectState_.openSaveAsNewModal = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            // Restore the active view from the pre-dirty snapshot when present —
            // covers widths / sort specs that got mutated in-place during this dirty
            // session. Then reload the session edit buffers from that restored view.
            ViewDefinition* mutableActiveForDiscard = ViewState.GetActiveViewMutable();
            if (mutableActiveForDiscard && d.viewsHasOriginalSnapshot) {
                *mutableActiveForDiscard = d.viewsOriginalSnapshot;
            }
            const ViewDefinition* restoreSource = mutableActiveForDiscard ? mutableActiveForDiscard : activeViewForGrid;
            d.editingColumnOrder = restoreSource->ColumnOrder;
            d.lastSyncedColumnOrder = restoreSource->ColumnOrder;
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewNameBuf, restoreSource->Name);
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlEditor.buf, restoreSource->Jql);
            // Re-seed the authoritative field selection from the restored view
            // (#views-field-uncheck).
            d.selectedFieldSet.clear();
            for (const auto& fieldId : restoreSource->Fields) {
                d.selectedFieldSet.insert(fieldId);
            }
            d.viewsDirty = false;
            d.viewSortDirty = false;
            d.viewsHasOriginalSnapshot = false;
            d.pendingViewStateSave = false;
            ViewState.BumpRevision(); // force grid to redraw columns in the stored order
            // If the unsaved edit included the QUERY (the omnibar / User-Info add-to-query
            // path — P2-H1), restoring the definition must also restore the visible rows:
            // re-adopt the saved JQL and re-run it, or the grid keeps showing the search.
            if (d.cfg.JqlQuery != restoreSource->Jql) {
                d.cfg.JqlQuery = restoreSource->Jql;
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            }
            SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.reverted_layout", "Reverted layout"),
                                                  restoreSource->Name, ToastType::Info, 1500);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        drawActiveProjectSaveAsNewModal(ctx);
    }

    const bool drewOfflineSection = DrawUnifiedOfflineQueuesPanel(app, d);
    if (drewOfflineSection) {
        ImGui::Spacing();
        ImGui::SeparatorText("Issue grid"); // P2-M15
        ImGui::Spacing();
    }
}

void SmatchetUI::drawActiveProjectSaveAsNewModal(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;
    char* s_newViewName = activeProjectState_.newViewNameBuf; // hoisted from `static char s_newViewName[128]`

    if (activeProjectState_.openSaveAsNewModal) {
        ImGui::OpenPopup("Save view as new");
        activeProjectState_.openSaveAsNewModal = false;
    }

    // Save-as-new modal: name input + Save / Cancel.
    if (ImGui::BeginPopupModal("Save view as new", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(s_newViewName, sizeof(activeProjectState_.newViewNameBuf), "%s (copy)",
                          activeViewForGrid->Name.c_str());
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::TextUnformatted("New view name");
        ImGui::SetNextItemWidth(300.0f);
        const bool committed =
            ImGui::InputText("##NewViewName", s_newViewName, sizeof(activeProjectState_.newViewNameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool disabled = s_newViewName[0] == '\0';
        if (disabled) {
            ImGui::BeginDisabled();
        }
        const bool saveClicked = ImGui::Button("Save") || committed;
        if (disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        if (saveClicked && !disabled) {
            // DEFERRED create (Pillar 3 crash fix): ViewState.Create here would reallocate
            // store.Views mid-frame, dangling ctx.activeViewForGrid for every section drawn
            // after this strip (table, sort block, post block) — the June 2026 view-create
            // crash. Copy the payload while the pointer is valid; applyPendingViewCreate
            // consumes the latch at the top of next frame (AdoptCfg reloads buffers + adopts
            // Jql/fields into cfg + saves, replacing the former inline post-create block).
            ViewDefinition created = *activeViewForGrid;
            created.Name = s_newViewName;
            created.Id.clear();
            if (!d.editingColumnOrder.empty()) {
                created.ColumnOrder = d.editingColumnOrder;
            }
            // Authoritative selection set, not the truncating buffer (#views-field-uncheck).
            const std::vector<std::string> editedFields =
                SmatchetViewsDashboardUiDetail::ToSortedVector(d.selectedFieldSet);
            if (!editedFields.empty()) {
                created.Fields = editedFields;
            }
            if (d.viewJqlEditor.buf[0]) {
                created.Jql = d.viewJqlEditor.buf;
            }
            d.viewsPendingCreate = true;
            d.viewsPendingCreatePayload = std::move(created);
            d.viewsPendingCreateToastTitle = "View created";
            d.viewsPendingCreateToastMs = 1500;
            d.viewsPendingCreateAdoptCfg = true;
            d.viewsDirty = false;
            d.viewsHasOriginalSnapshot = false;
            d.pendingViewStateSave = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
