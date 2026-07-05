#include "SmatchetUI.h"
#include "SmatchetGridPaneWindows.h" // detail::PaneViewSelfRepairAllowed (HIGH-1) + ChoosePaneColumnsSource
#include "SmatchetGridUiSupport.h"
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
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"
#include "Ui/SmatchetTooltipWheelRouter.h"
#include "UiPerfMonitor.h"

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

static bool IsPersistableSortDirection(ImGuiSortDirection dir) {
    return dir == ImGuiSortDirection_Ascending || dir == ImGuiSortDirection_Descending;
}

static bool WindowBelongsToTableWheelScope(ImGuiTable* table, ImGuiWindow* window) {
    if (!table || !window) {
        return false;
    }
    ImGuiWindow* inner = table->InnerWindow;
    ImGuiWindow* outer = table->OuterWindow;
    return window == inner || window == outer || (inner && ImGui::IsWindowChildOf(window, inner, false, false)) ||
           (outer && ImGui::IsWindowChildOf(window, outer, false, false));
}

static bool TableOwnsCurrentWheelRoute(ImGuiTable* table) {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !table || smatchet::ui::HasOpenScrollableTooltip()) {
        return false;
    }

    ImGuiWindow* wheelOwner = g->WheelingWindow ? g->WheelingWindow : g->HoveredWindow;
    if (!wheelOwner) {
        return false;
    }
    return WindowBelongsToTableWheelScope(table, wheelOwner);
}

/** When vertically at top/bottom (or no vertical scroll), map mouse wheel to horizontal scroll; first N wheel ticks at
 * each end ignored (configured by GridEndWheelSwallowsBeforeHorizontal). Per-pane hysteresis (Slice 2). */
static void RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGuiTable* table, UiDrawSession& d, GridPane& pane) {
    const int endWheelSwallowsBeforeHorizontal = (std::max)(0, d.cfg.GridEndWheelSwallowsBeforeHorizontal);

    if (smatchet::ui::RouteWheelToScrollableTooltip()) {
        return;
    }

    if (!table) {
        return;
    }
    ImGuiWindow* inner = table->InnerWindow;
    ImGuiWindow* outer = table->OuterWindow;
    const ImGuiIO& io = ImGui::GetIO();

    if (!inner || std::abs(io.MouseWheel) <= 0.0f || std::abs(io.MouseWheelH) >= 0.0001f ||
        inner->ScrollMax.x <= 0.0f) {
        return;
    }
    if (!TableOwnsCurrentWheelRoute(table)) {
        return;
    }

    const float eps = 1.0f;
    const bool atBottom = inner->Scroll.y >= inner->ScrollMax.y - eps;
    const bool atTop = inner->Scroll.y <= eps;
    const bool noVerticalScroll = inner->ScrollMax.y <= eps;

    if (outer) {
        const ImRect tableRect = outer->Rect();
        if (!ImGui::IsMouseHoveringRect(tableRect.Min, tableRect.Max, false)) {
            return;
        }
    }

    if (!atBottom) {
        pane.gridBottomHorizontalWheelSwallowsRemaining = 0;
    }
    if (!atTop) {
        pane.gridTopHorizontalWheelSwallowsRemaining = 0;
    }

    bool allowRoute = false;
    if (noVerticalScroll) {
        allowRoute = true;
    } else if (atBottom) {
        if (pane.gridBottomHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++pane.gridBottomHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    } else if (atTop) {
        if (pane.gridTopHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++pane.gridTopHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    }

    if (!allowRoute) {
        return;
    }

    const float wheelToScrollX = -io.MouseWheel * (ImGui::GetTextLineHeightWithSpacing() * 3.0f);
    float targetX = inner->Scroll.x + wheelToScrollX;
    if (targetX < 0.0f) {
        targetX = 0.0f;
    }
    if (targetX > inner->ScrollMax.x) {
        targetX = inner->ScrollMax.x;
    }
    ImGui::SetScrollX(inner, targetX);
}

// Rebuild ONE PANE's cached sort-order + filter projection (`pane.cachedSortedIndices` →
// `pane.filteredIndices`). Extracted from drawActiveProjectGridSort so that helper stays
// under the function-size cap; runs only when the projection is dirty AND the streaming
// debounce permits. Per-pane caches (Slice 2) keep a steady-state re-render an O(1) hit.
static void RebuildGridSortAndFilterProjection(GridPane& pane, ImGuiTableSortSpecs* sortSpecs,
                                               const std::vector<CachedTicket>& tickets,
                                               const std::vector<TicketGridColumn>& columns,
                                               const TrackerFieldCatalogIndex& catalogIndex,
                                               const std::string& fingerprint, std::uint64_t activeTicketsRevision,
                                               std::uint64_t catalogRevision, char* lastFilter, size_t lastFilterCap) {
    pane.lastGridSortAt = std::chrono::steady_clock::now();

    // 1. Run Sort Spec / Order Indices
    pane.cachedSortedIndices.resize(tickets.size());
    for (size_t i = 0; i < tickets.size(); ++i) {
        pane.cachedSortedIndices[i] = i;
    }

    if (sortSpecs && sortSpecs->SpecsCount > 0 && sortSpecs->Specs != nullptr) {
        // Resolve field meta once per sort spec outside the comparator (§3.4 item 55):
        // avoids catalogIndex.Find() on every pair comparison in O(N log N) sort.
        struct SortKey {
            const TicketGridColumn* col = nullptr;
            const TrackerField* fieldMeta = nullptr;
            int dir = 1;
        };
        std::vector<SortKey> sortKeys;
        for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
            if (!IsPersistableSortDirection(spec.SortDirection))
                continue;
            const int ci = spec.ColumnIndex;
            if (ci < 0 || ci >= static_cast<int>(columns.size()))
                continue;
            SortKey sk;
            sk.col = &columns[static_cast<size_t>(ci)];
            sk.fieldMeta = catalogIndex.Find(sk.col->FieldId);
            sk.dir = (spec.SortDirection == ImGuiSortDirection_Ascending) ? 1 : -1;
            sortKeys.push_back(sk);
        }
        const std::vector<CachedTicket>* ticketsPtr = &tickets;
        std::stable_sort(pane.cachedSortedIndices.begin(), pane.cachedSortedIndices.end(),
                         [ticketsPtr, &sortKeys](size_t ia, size_t ib) {
                             const auto& tix = *ticketsPtr;
                             for (const auto& sk : sortKeys) {
                                 if (sk.col->ColumnKind == TicketGridColumn::Kind::Id) {
                                     const bool less = CompareIssueKeyNatural(tix[ia].id, tix[ib].id);
                                     if (less)
                                         return sk.dir > 0;
                                     if (!CompareIssueKeyNatural(tix[ib].id, tix[ia].id))
                                         continue;
                                     return sk.dir < 0;
                                 }
                                 // Zero-copy refs (Phase 5 pull-forward): GetFieldValueRef avoids the two
                                 // std::string copies GetFieldValue made on every comparison inside stable_sort.
                                 const std::string& aVal = tix[ia].GetFieldValueRef(sk.col->FieldId);
                                 const std::string& bVal = tix[ib].GetFieldValueRef(sk.col->FieldId);
                                 const int cmp =
                                     CompareFieldValuesForSort(sk.col->FieldId, sk.fieldMeta, aVal, bVal, sk.dir);
                                 if (cmp != 0)
                                     return (cmp * sk.dir) < 0;
                             }
                             return ia < ib;
                         });
    }

    pane.cachedSortFingerprint = fingerprint;
    pane.cachedSortValid = true;
    pane.cachedSortTicketsRevision = activeTicketsRevision;
    pane.cachedSortCatalogRevision = catalogRevision;

    // 2. Run Filter and rebuild pane.filteredIndices
    pane.filteredIndices.clear();
    auto checkMatch = [&](size_t idx) {
        if (idx >= tickets.size())
            return false;
        if (pane.gridFilterBuf[0] == '\0')
            return true;
        const auto& t = tickets[idx];
        if (ContainsCaseInsensitive(t.id, pane.gridFilterBuf))
            return true;
        if (ContainsCaseInsensitive(t.GetFieldValue("summary"), pane.gridFilterBuf))
            return true;
        return false;
    };

    for (size_t idx : pane.cachedSortedIndices) {
        if (checkMatch(idx)) {
            pane.filteredIndices.push_back(idx);
        }
    }

    // snprintf guarantees null-termination and avoids the strncpy
    // truncation warning when the source fills the buffer exactly.
    std::snprintf(lastFilter, lastFilterCap, "%s", pane.gridFilterBuf);
}

// Mirror header-click sort changes back onto the active view definition, marking it
// dirty only when the rules actually changed (avoids a false-dirty on startup or when
// the active view is swapped). Extracted from drawActiveProjectGridSort for the cap.
static void SyncHeaderSortClicksToView(UiDrawSession& d, ImGuiTableSortSpecs* sortSpecs,
                                       const std::vector<TicketGridColumn>& columns, ViewDefinition& activeView) {
    std::vector<ViewSortSpec> newSpecs;
    for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
        const ImGuiTableColumnSortSpecs& sp = sortSpecs->Specs[s];
        if (sp.ColumnIndex >= 0 && sp.ColumnIndex < static_cast<int>(columns.size()) &&
            IsPersistableSortDirection(sp.SortDirection)) {
            ViewSortSpec vs;
            vs.ColumnKey = columns[sp.ColumnIndex].Key;
            vs.Direction = static_cast<int>(sp.SortDirection);
            newSpecs.push_back(vs);
        }
    }

    // Only mark dirty if the sorting rules actually changed (prevent startup/view-switch false dirty)
    bool changed = (newSpecs.size() != activeView.SortSpecs.size());
    if (!changed) {
        for (size_t i = 0; i < newSpecs.size(); ++i) {
            if (newSpecs[i].ColumnKey != activeView.SortSpecs[i].ColumnKey ||
                newSpecs[i].Direction != activeView.SortSpecs[i].Direction) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        // Snapshot pre-change view so Discard can revert sort + widths
        // + column order + buffers all at once.
        SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, activeView);
        activeView.SortSpecs = std::move(newSpecs);
        d.viewSortDirty = true;
        d.viewsDirty = true;
    }
}

// Shift+Space gesture: promote the row containing the active cell to a whole-row
// selection (added to whatever is already selected). Extracted from
// drawActiveProjectGridRectSelKeys to keep that helper under the function-size cap.
template <class RectSelT>
static void PromoteActiveRowToSelection(GridPane& pane, RectSelT& sel, const std::vector<CachedTicket>& tickets,
                                        std::uint64_t gridSortSig) {
    int activeRow = -1;
    if (sel.PrimaryRow >= 0) {
        activeRow = sel.PrimaryRow;
    } else if (sel.Active) {
        activeRow = sel.AnchorRow;
    } else if (!pane.gridState.ActiveIssueId.empty()) {
        const auto& indices = pane.filteredIndices;
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t ti = indices[i];
            if (ti < tickets.size() && tickets[ti].id == pane.gridState.ActiveIssueId) {
                activeRow = static_cast<int>(i);
                break;
            }
        }
    }
    if (activeRow >= 0) {
        sel.Rows.insert(activeRow);
        sel.PrimaryRow = activeRow;
        sel.Active = false;
        sel.SortSignature = gridSortSig;
    }
}

} // namespace

#if defined(SMATCHET_BUILD_UI_TESTS)
extern "C" void SmatchetUiTestRouteActiveProjectGridWheelForCurrentTable(UiDrawSession* d, GridPane* pane) {
    if (!d || !pane) {
        return;
    }
    RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGui::GetCurrentTable(), *d, *pane);
}
#endif

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

// Pane-strip "+" / "▾" split-button (pane-backend-picker Slice 2).
// Bare "+" always duplicates the focused pane (no backend arg = same-backend path).
// "▾" caret renders only when ≥2 backends have minimum credentials present; it opens
// a popup that lists each credentialed backend. Selecting a backend (with no specific
// view) opens its default/active view; an optional view submenu lists saved views.
static void DrawNewPaneMenu(UiDrawSession& d, const GridPane& pane,
                            const std::unordered_map<std::string, ViewWorkspaceState>& diskBackends) {
    if (ImGui::SmallButton("+##PaneAdd")) {
        d.paneAddRequest.sourceId = pane.id;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T("pane.add.tooltip",
                                                        "New grid pane (duplicates this pane; dock it as a tab or "
                                                        "drag its tab to an edge for a side-by-side split)"));
    }

    const std::vector<std::string>& knownKeys = ConfigManager::KnownBackendKeys();
    int credCount = 0;
    for (const auto& key : knownKeys) {
        if (ConfigManager::BackendCredentialsPresent(d.cfg, key)) {
            ++credCount;
        }
    }
    if (credCount < 2) {
        return;
    }

    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::SmallButton("\xe2\x96\xbe##PaneBackendPicker")) {
        ImGui::OpenPopup("##PaneNewOnBackend");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T("pane.backend.picker.tooltip",
                                                        "Open a new pane on a different tracker backend"));
    }

    if (ImGui::BeginPopup("##PaneNewOnBackend")) {
        for (const auto& key : knownKeys) {
            if (!ConfigManager::BackendCredentialsPresent(d.cfg, key)) {
                continue;
            }
            const auto it = diskBackends.find(key);
            const bool hasViews = (it != diskBackends.end() && !it->second.Views.empty());
            const std::string label = "New " + key + " pane";

            if (hasViews && ImGui::BeginMenu(label.c_str())) {
                const ViewWorkspaceState& ws = it->second;
                for (const auto& v : ws.Views) {
                    const bool isActive = (v.Id == ws.ActiveViewId);
                    if (ImGui::MenuItem(v.Name.c_str(), nullptr, isActive)) {
                        d.paneAddRequest.sourceId = pane.id;
                        d.paneAddRequest.targetBackendKey = key;
                        d.paneAddRequest.targetViewId = v.Id;
                    }
                }
                ImGui::EndMenu();
            } else if (!hasViews) {
                if (ImGui::MenuItem(label.c_str())) {
                    d.paneAddRequest.sourceId = pane.id;
                    d.paneAddRequest.targetBackendKey = key;
                }
            }
        }
        ImGui::EndPopup();
    }
}

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
    if (!embedded) {
        prepareTopLevelWindow(d, "active", 900.0f, 620.0f, wantFocus);
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
        if (pane.id == "main") {
            paneFlags |= ImGuiWindowFlags_NoTitleBar;
        }
        if (!ImGui::Begin(pane.cachedWindowName.c_str(), paneOpen, paneFlags)) {
            ImGui::End();
            return;
        }
        repairTopLevelWindow(d, "active", 420.0f, 300.0f);
        if (wantFocus) {
            ImGui::SetWindowFocus();
            d.requestActiveProjectFocus = false;
        }
        // Report window focus to the pane host (consumed after the pane loop). Focus is
        // what flips which pane drives the single Slice-2 live context.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            d.paneWindowFocusedThisFrame = pane.id;
        }
        // Pane strip: bare "+" duplicates this pane; "▾" opens a backend picker when ≥2
        // backends are credentialed. The host applies requests after the loop (never
        // mutates gridPanes mid-iteration). Close rides the window's tab X.
        DrawNewPaneMenu(d, pane, ViewState.GetDiskBackends());
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
    const TrackerFieldCatalogIndex& catalogIndex = *gridFrameCtx_.catalogIndex;
    // Cross-backend cold-start defense (Slice 4): the pane's OWN view resolved by its context
    // sync, so resolvePaneColumns can build the pane's OWN column SET even when the focused
    // ViewState bucket can't see it and no session capture exists yet (after a restart) —
    // closing the frozen-capture hole that left a restarted unfocused cross-backend pane
    // rendering the focused view's columns until first focus.
    std::shared_ptr<const ViewDefinition> paneOwnResolvedView =
        pane.focused ? nullptr : app.GetPaneResolvedView(pane.id);
    const std::vector<TicketGridColumn>& columns =
        resolvePaneColumns(pane, catalogIndex, activeViewForGrid, paneOwnResolvedView.get());

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
                                                                    const ViewDefinition* paneOwnResolvedView) {
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
            pane.cachedColumnsCatalogRevision != gridFrameCtx_.catalogRevision ||
            pane.cachedColumnsViewsRevision != gridFrameCtx_.viewsRevision) {
            pane.cachedColumns = TicketGridColumnsBuilder::Build(*paneOwnResolvedView, catalogIndex);
            pane.cachedColumnsCatalogRevision = gridFrameCtx_.catalogRevision;
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
    if (!pane.cachedColumnsValid || pane.cachedColumnsCatalogRevision != gridFrameCtx_.catalogRevision ||
        pane.cachedColumnsViewsRevision != gridFrameCtx_.viewsRevision || pane.cachedColumnsViewId != paneView->Id) {
        pane.cachedColumns = (source == PaneColumnsSource::SharedActive)
                                 ? gridFrameCtx_.columns
                                 : TicketGridColumnsBuilder::Build(*paneView, catalogIndex);
        pane.cachedColumnsCatalogRevision = gridFrameCtx_.catalogRevision;
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

// The TicketGrid BeginTable block: the five grid section helpers plus the post-layout inside-table
// click detection. Split out of drawActiveProjectWindow under the function-size cap; behaviour-
// identical (the inside-table hit flags write back through the ctx references).
void SmatchetUI::drawActiveProjectTable(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    const std::vector<TicketGridColumn>& columns = ctx.columns;

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                                       ImGuiTableFlags_SortTristate | ImGuiTableFlags_NoSavedSettings;

    ImGui::Separator();

    (void)smatchet::ui::RouteWheelToScrollableTooltip();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));
    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        // Scenario-driven scroll: honor the target set by ScenarioRunner::Tick so automated
        // tests can drive the grid position without human input. Scenarios address "the
        // grid" — the focused pane.
        if (d.scenarioScrollActive && d.scenarioScrollTarget >= 0 && ctx.pane.focused) {
            ImGui::SetScrollY(static_cast<float>(d.scenarioScrollTarget));
        }
        // Sibling horizontal hook for interaction-injection scenarios. The table inner
        // window (current after BeginTable with ScrollX) is the X-scroll target, mirroring
        // the SetScrollY path above. Gated on the focused pane only.
        if (d.scenarioScrollTargetX >= 0 && ctx.pane.focused) {
            ImGui::SetScrollX(static_cast<float>(d.scenarioScrollTargetX));
        }
        drawActiveProjectGridSetup(ctx);

        drawActiveProjectGridSort(ctx);

        drawActiveProjectGridRows(ctx);

        drawActiveProjectGridNewIssue(ctx);

        drawActiveProjectGridPost(ctx);
        // After full table layout: union outer + inner clip so empty scroll body
        // and padding still count as "inside grid" (OuterRect alone missed H5).
        if (ImGuiContext* g = ImGui::GetCurrentContext()) {
            if (ImGuiTable* tb = g->CurrentTable) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImRect hit(tb->OuterRect);
                    hit.Add(tb->InnerClipRect);
                    if (hit.Contains(ImGui::GetIO().MousePos)) {
                        ctx.ticketGridLeftClickInsideTableHit = true;
                        ctx.rectCellClickedThisFrame = true;
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
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
    if ((d.viewsDirty || d.viewSortDirty) && activeViewForGrid) {
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
            ImGui::OpenPopup("Save view as new");
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
        ImGui::SeparatorText("Ticket grid");
        ImGui::Spacing();
    }
}

void SmatchetUI::drawActiveProjectSaveAsNewModal(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;
    char* s_newViewName = activeProjectState_.newViewNameBuf; // hoisted from `static char s_newViewName[128]`

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

void SmatchetUI::drawActiveProjectGridSetup(ActiveProjectDrawCtx& ctx) {
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    const TrackerFieldCatalogIndex& catalogIndex = ctx.catalogIndex;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;
    const bool gridSortEnvironmentChanged = ctx.gridSortEnvironmentChanged;

    SMATCHET_UI_PERF_SCOPE("activeProject:grid.setup");
    // Column WIDTHS ownership gate (Slice 4 — same strict-Id discipline as the sort-apply
    // and column-set gates): a fallback-resolved view (cross-backend unfocused pane) must
    // NOT push ITS widths onto this pane by shared column-key match — a non-owned pane uses
    // its own captured widths or the defaults, never the fallback's (review #986 class).
    const bool widthsArePanesOwn = activeViewForGrid && activeViewForGrid->Id == ctx.pane.viewId;
    // Materialise column widths once (§3.1 item 56): avoids ColumnWidths.find per column per frame.
    std::vector<float> colWidths(columns.size());
    for (size_t ci = 0; ci < columns.size(); ++ci) {
        float w = (columns[ci].ColumnKind == TicketGridColumn::Kind::Id) ? 90.0f : 180.0f;
        if (widthsArePanesOwn) {
            const auto wIt = activeViewForGrid->ColumnWidths.find(columns[ci].Key);
            if (wIt != activeViewForGrid->ColumnWidths.end() && wIt->second > 0.0f) {
                w = wIt->second;
            }
        }
        colWidths[ci] = w;
    }
    for (size_t ci = 0; ci < columns.size(); ++ci) {
        ImGui::TableSetupColumn(columns[ci].Label.c_str(), ImGuiTableColumnFlags_WidthFixed, colWidths[ci]);
    }
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int hci = 0; hci < static_cast<int>(columns.size()); ++hci) {
        ImGui::TableSetColumnIndex(hci);
        const TicketGridColumn& hcol = columns[static_cast<size_t>(hci)];

        // Match ImGui::TableHeadersRow(): TableHeader IDs must be unique even when labels repeat.
        ImGui::PushID(hci);
        ImGui::TableHeader(hcol.Label.c_str());

        const TrackerField* hdrMeta =
            (hcol.ColumnKind == TicketGridColumn::Kind::Id) ? nullptr : catalogIndex.Find(hcol.FieldId);
        DrawTicketGridHeaderContextMenu(hcol, hdrMeta);
        ImGui::PopID();
    }

    // Apply persisted sort from the view only when the grid context changes or the Sort By popup edits it.
    // Ownership-gated (review HIGH-2): a fallback-resolved view (cross-backend unfocused
    // pane) must never push ITS SortSpecs onto this pane — shared column keys like
    // status/summary would match and re-sort the wrong grid. Same strict-Id discipline
    // as the sort WRITE mirror in drawActiveProjectGridSort.
    if (activeViewForGrid && activeViewForGrid->Id == ctx.pane.viewId) {
        ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        const bool hasPersistedSort = !activeViewForGrid->SortSpecs.empty();
        const bool shouldApplyPersistedSort = specs && (gridSortEnvironmentChanged || ctx.pane.forceApplySortSpecs);
        if (shouldApplyPersistedSort) {
            // Re-applying whenever ImGui briefly reports zero specs can override a header click that is
            // cycling through tri-state sort directions.
            for (int c = 0; c < static_cast<int>(columns.size()); ++c) {
                ImGui::TableSetColumnSortDirection(c, ImGuiSortDirection_None, false);
            }
            if (hasPersistedSort) {
                int appliedSortCount = 0;
                for (const ViewSortSpec& vs : activeViewForGrid->SortSpecs) {
                    const ImGuiSortDirection direction = static_cast<ImGuiSortDirection>(vs.Direction);
                    if (!IsPersistableSortDirection(direction))
                        continue;
                    int colIndex = -1;
                    auto colIt = std::find_if(columns.begin(), columns.end(),
                                              [&](const auto& col) { return col.Key == vs.ColumnKey; });
                    if (colIt != columns.end()) {
                        colIndex = static_cast<int>(std::distance(columns.begin(), colIt));
                    }
                    if (colIndex >= 0) {
                        ImGui::TableSetColumnSortDirection(colIndex, direction, appliedSortCount > 0);
                        ++appliedSortCount;
                    }
                }
            }
        }
    }
}

void SmatchetUI::drawActiveProjectGridSort(ActiveProjectDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    GridPane& pane = ctx.pane;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    const TrackerFieldCatalogIndex& catalogIndex = ctx.catalogIndex;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;
    const bool gridSortEnvironmentChanged = ctx.gridSortEnvironmentChanged;
    char* lastFilter = pane.lastFilterBuf; // per-pane (was a SmatchetUI member; the filter is per-pane now)

    SMATCHET_UI_PERF_SCOPE("activeProject:grid.sort");
    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if (gridSortEnvironmentChanged || pane.forceApplySortSpecs) {
        pane.cachedSortValid = false;
        pane.forceApplySortSpecs = false;
    }
    if (sortSpecs && sortSpecs->SpecsDirty) {
        pane.cachedSortValid = false;
        sortSpecs->SpecsDirty = false;

        // Sync header clicks back to View definition (focused pane only — the
        // view-definition mirror is session-level unsaved-edit state). Gate on LIVE
        // focus — this frame's window-focus report — not last frame's pane.focused:
        // a sort click into a not-yet-focused pane processes on the SAME frame ImGui
        // moves focus, and the stale flag dropped the mirror while SpecsDirty was
        // already cleared above, so the sort applied visually but never reached the
        // view (review HIGH-3). The mirror must ALSO target the pane's OWN view:
        // on that same focus-click frame a cross-backend pane renders the
        // fallback-resolved view (the other backend's active view — HIGH-1 refuses
        // the identity write but still returns the fallback), and mirroring into it
        // would durably misdirect the sort into the other pane's view definition
        // (delta-review HIGH, second-order of the live-focus widening). The strict
        // Id equality is sufficient on its own: resolvePaneView's self-repair runs
        // earlier in this same draw and rewrites every repair-eligible pane's
        // viewId to the rendered view's Id, so any pane still mismatching here is
        // exactly a repair-refused cross-backend pane (including the empty-viewId
        // one persisted from an empty bucket) — the states that must not mirror.
        const bool paneLiveFocused = pane.focused || d.paneWindowFocusedThisFrame == pane.id;
        const bool viewIsPanesOwn = activeViewForGrid && activeViewForGrid->Id == pane.viewId;
        if (paneLiveFocused && viewIsPanesOwn) {
            SyncHeaderSortClicksToView(d, sortSpecs, columns, *activeViewForGrid);
        }
    }
    // Keyed on the PANE's snapshot revision (not the live revision) so a non-focused
    // pane's frozen snapshot doesn't re-sort on every background sync tick.
    const std::uint64_t activeTicketsRevision = pane.snapshotRevision;
    if (activeTicketsRevision != pane.cachedSortTicketsRevision) {
        pane.cachedSortValid = false;
    }
    const std::uint64_t catalogRevision = app.GetFieldCatalogRevision();
    if (catalogRevision != pane.cachedSortCatalogRevision) {
        pane.cachedSortValid = false;
    }

    std::string fingerprint;
    if (sortSpecs && sortSpecs->SpecsCount > 0 && sortSpecs->Specs != nullptr) {
        fingerprint.reserve(static_cast<size_t>(sortSpecs->SpecsCount) * 48);
        for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
            if (!IsPersistableSortDirection(spec.SortDirection))
                continue;
            fingerprint += std::to_string(spec.ColumnIndex);
            fingerprint.push_back(':');
            fingerprint += std::to_string(static_cast<int>(spec.SortDirection));
            fingerprint.push_back(':');
            fingerprint += std::to_string(static_cast<int>(spec.SortOrder));
            fingerprint.push_back('|');
        }
    }

    bool filterChanged = (std::strcmp(lastFilter, pane.gridFilterBuf) != 0);
    if (filterChanged) {
        pane.gridState.RectSel.ClearAll();
    }

    // Treat sort+filter as one cached projection with a dirty flag and refresh it at a bounded interval (500ms)
    // during streaming
    bool needsProjectionRefresh = false;
    if (!pane.cachedSortValid || pane.cachedSortFingerprint != fingerprint ||
        pane.cachedSortedIndices.size() != tickets.size() || activeTicketsRevision != pane.cachedSortTicketsRevision ||
        catalogRevision != pane.cachedSortCatalogRevision || filterChanged) {
        needsProjectionRefresh = true;
    }

    bool okToRefreshProjection = true;
    if (app.IsStreamingSyncActive() && needsProjectionRefresh) {
        // Debounce cheap reshuffles while tickets stream, but never delay a user-driven sort change:
        // header/persist fingerprint differs from last applied projection → apply immediately (fixes header
        // clicks feeling stuck while Sort By still worked due to slower interaction pacing).
        const bool sortKeyChanged = (fingerprint != pane.cachedSortFingerprint);
        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - pane.lastGridSortAt).count();
        if (!sortKeyChanged && elapsedMs < 500) {
            okToRefreshProjection = false;
        }
    }

    if (needsProjectionRefresh && okToRefreshProjection) {
        RebuildGridSortAndFilterProjection(pane, sortSpecs, tickets, columns, catalogIndex, fingerprint,
                                           activeTicketsRevision, catalogRevision, lastFilter,
                                           sizeof(pane.lastFilterBuf));
    }
}

void SmatchetUI::drawActiveProjectGridRows(ActiveProjectDrawCtx& ctx) {
    AppController& app = ctx.app;
    GridPane& pane = ctx.pane;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    const bool readOnlyMode = ctx.readOnlyMode;
    std::uint64_t& gridSortSig = ctx.gridSortSig;

    // Rectangular selection invalidation: anchor/extent are expressed in
    // current sort-order row indices, so any change to sort order or ticket
    // set must clear the selection to avoid nonsense highlights / copies.
    gridSortSig = ComputeGridSortSignature(pane.cachedSortFingerprint, pane.snapshotRevision, tickets.size());
    if (pane.gridState.RectSel.HasAnySelection() && pane.gridState.RectSel.SortSignature != gridSortSig) {
        pane.gridState.RectSel.ClearAll();
    }

    SMATCHET_UI_PERF_SCOPE("activeProject:grid.rows");
    const size_t oldFilteredCount = pane.filteredIndices.size();
    pane.filteredIndices.erase(std::remove_if(pane.filteredIndices.begin(), pane.filteredIndices.end(),
                                              [&](size_t idx) { return idx >= tickets.size(); }),
                               pane.filteredIndices.end());
    if (pane.filteredIndices.size() != oldFilteredCount) {
        pane.gridState.RectSel.ClearAll();
    }
    const std::vector<size_t>& indicesToUse = pane.filteredIndices;

    // Per-frame cache: raw status string → highlight color. Avoids ToLowerAsciiCopy +
    // 4 find() per visible row — each unique status value is lowercased only once.
    std::unordered_map<std::string, ImVec4> statusColorMap;
    auto StatusRowColor = [&](const std::string& raw) -> ImVec4 {
        const auto it = statusColorMap.find(raw);
        if (it != statusColorMap.end())
            return it->second;
        const std::string lower = ToLowerAsciiCopy(raw);
        ImVec4 color(0, 0, 0, 0);
        if (lower.find("done") != std::string::npos || lower.find("resolved") != std::string::npos)
            color = SmatchetTheme::Colors::StatusDone;
        else if (lower.find("progress") != std::string::npos)
            color = SmatchetTheme::Colors::StatusInProgress;
        else if (lower.find("todo") != std::string::npos || lower.find("open") != std::string::npos ||
                 lower.find("backlog") != std::string::npos)
            color = SmatchetTheme::Colors::StatusToDo;
        else if (lower.find("block") != std::string::npos)
            color = SmatchetTheme::Colors::StatusBlocked;
        statusColorMap.emplace(raw, color);
        return color;
    };

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(indicesToUse.size()));
    while (clipper.Step()) {
        for (int clippedRow = clipper.DisplayStart; clippedRow < clipper.DisplayEnd; ++clippedRow) {
            const size_t r = static_cast<size_t>(clippedRow);
            const size_t ticketIndex = indicesToUse[r];
            if (ticketIndex >= tickets.size())
                continue;
            const CachedTicket& ticket = tickets[ticketIndex];
            bool isActiveIssue = (pane.gridState.ActiveIssueId == ticket.id);
            const bool idKeySelectableSelected = pane.gridState.RectSel.RowSelected(clippedRow);
            const bool activeIssueWasThisRow = isActiveIssue;
            if (isActiveIssue && !readOnlyMode) {
                app.WarmIssueEditMetaAsync(ticket.id);
            }
            thread_local char rowPerfLabel[288];
            const char* rowPerfScopeName = nullptr;
            if (isActiveIssue) {
                std::snprintf(rowPerfLabel, sizeof(rowPerfLabel), "activeProject:row[%zu] %s", r, ticket.id.c_str());
                rowPerfLabel[sizeof(rowPerfLabel) - 1] = '\0';
                rowPerfScopeName = rowPerfLabel;
            }
            SMATCHET_UI_PERF_SCOPE(rowPerfScopeName);
            // One line of text + table cell Y padding (compact; close to a single-line InputText without
            // TextLineHeightWithSpacing’s extra line gap). Do not use GetContentRegionAvail().y for row height.
            const float kTicketGridRowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
            ImGui::TableNextRow(0, kTicketGridRowH);

            // Status-based Row Highlighting — color looked up from a per-frame cache so
            // ToLowerAsciiCopy + 4 string::find are paid once per unique status value,
            // not once per visible row. Cache is a lambda-captured unordered_map.
            const ImVec4 statusColor = StatusRowColor(ticket.GetFieldValue("status"));
            if (statusColor.w > 0.0f) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(statusColor.x, statusColor.y, statusColor.z, 0.12f)));
            }

            for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
                drawActiveProjectGridCell(ctx, ticket, columns[static_cast<size_t>(colIndex)], clippedRow, colIndex,
                                          idKeySelectableSelected, activeIssueWasThisRow, kTicketGridRowH);
            }
        }
    }
}

void SmatchetUI::drawActiveProjectGridCell(ActiveProjectDrawCtx& ctx, const CachedTicket& ticket,
                                           const TicketGridColumn& column, int clippedRow, int colIndex,
                                           bool idKeySelectableSelected, bool activeIssueWasThisRow, float rowHeight) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    GridPane& pane = ctx.pane;
    const bool readOnlyMode = ctx.readOnlyMode;

    if (!ImGui::TableSetColumnIndex(colIndex)) {
        if (!pane.gridState.IsEditingField(ticket.id, column.FieldId)) {
            // Horizontally clipped columns must still contribute layout height or row height
            // tracks only visible cells (ImGui::TableSetColumnIndex doc).
            ImGui::Dummy(ImVec2(1.0f, rowHeight));
            return;
        }
    }

    // Captured before any widget advances the cursor so rect-
    // select hit boxes cover the entire column cell area
    // (not just the rendered text / widget extent).
    const ImVec2 cellOriginForSel = ImGui::GetCursorScreenPos();
    const float cellWidthForSel = ImGui::GetContentRegionAvail().x;

    if (column.ColumnKind == TicketGridColumn::Kind::Id) {
        ImVec2 cellGroupMin(0.0f, 0.0f);
        ImVec2 cellGroupMax(0.0f, 0.0f);
        ImGui::BeginGroup();
        if (ImGui::Selectable(ticket.id.c_str(), idKeySelectableSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (!ImGuiEffectiveKeyCtrl()) {
                pane.gridState.SetActiveIssue(ticket.id);
            }
            if (ImGui::IsMouseDoubleClicked(0)) {
                const std::string url = app.BuildIssueBrowseUrl(d.cfg, ticket.id);
                app.OpenUrl(url);
            }
        }
        ImGui::EndGroup();
        cellGroupMin = ImGui::GetItemRectMin();
        cellGroupMax = ImGui::GetItemRectMax();
        DrawGridCellRightClickPopups(BuildCellKey(ticket.id, "id"), ticket.id, std::string(), column.Label, ticket.id,
                                     std::string(), &app, &d, readOnlyMode, &ticket);
        handleActiveProjectCellRectSel(ctx, clippedRow, colIndex, cellOriginForSel.x, cellWidthForSel, cellGroupMin.y,
                                       cellGroupMax.y, true, ticket.id, activeIssueWasThisRow);
        return;
    }

    drawActiveProjectGridValueCell(ctx, ticket, column, clippedRow, colIndex, cellOriginForSel.x, cellWidthForSel);
}

// Data (non-Id) grid-cell render: write-state badge + saving/editable value + trailing rect-select.
// Split out of drawActiveProjectGridCell under the function-size cap; behaviour-identical (cell
// geometry arrives as floats and the rect-select hit flags write back through the ctx references).
void SmatchetUI::drawActiveProjectGridValueCell(ActiveProjectDrawCtx& ctx, const CachedTicket& ticket,
                                                const TicketGridColumn& column, int clippedRow, int colIndex,
                                                float cellOriginX, float cellWidth) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const TrackerFieldCatalogIndex& catalogIndex = ctx.catalogIndex;
    const bool readOnlyMode = ctx.readOnlyMode;
    std::vector<PendingFieldEdit>& pendingEdits = ctx.pendingEdits;

    ImVec2 cellGroupMin(0.0f, 0.0f);
    ImVec2 cellGroupMax(0.0f, 0.0f);

    const std::string currentValue = ticket.GetFieldValue(column.FieldId);
    const TrackerField* fieldMeta = catalogIndex.Find(column.FieldId);
    const float cellStartY = ImGui::GetCursorScreenPos().y;
    const float cellRightX = ImGui::GetCursorScreenPos().x + cellWidth;
    const float valueAvailWidth = cellRightX - ImGui::GetCursorScreenPos().x;
    const std::string cellKey = BuildCellKey(ticket.id, column.FieldId);
    // Skip map lookup when feedback map is empty (common case) — avoids the hash (§3.1 item 57).
    const auto feedbackIt = d.cellFeedbackByKey.empty() ? d.cellFeedbackByKey.end() : d.cellFeedbackByKey.find(cellKey);
    const bool isSavingThisCell =
        feedbackIt != d.cellFeedbackByKey.end() && feedbackIt->second.State == CellWriteState::Saving;

    bool showBadge = false;
    ImVec4 badgeColor(1.0f, 1.0f, 1.0f, 1.0f);
    std::string badgeText;
    std::string badgeTooltip;
    if (feedbackIt != d.cellFeedbackByKey.end() && feedbackIt->second.State == CellWriteState::Error &&
        !feedbackIt->second.Message.empty()) {
        showBadge = true;
        badgeColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        badgeText = "!";
        badgeTooltip = feedbackIt->second.Message;
    } else if (feedbackIt != d.cellFeedbackByKey.end() && feedbackIt->second.State == CellWriteState::Success) {
        showBadge = true;
        badgeColor = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
        badgeText = "OK";
        badgeTooltip = "Saved";
    }

    if (isSavingThisCell) {
        showBadge = true;
        badgeColor = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
        badgeText = "...";
        badgeTooltip = "Saving...";
        ImGui::BeginGroup();
        const std::string saveDisplay = DisplayValueForTrackerDateField(
            column.FieldId, fieldMeta, currentValue, d.cfg.DateFormatOption, d.cfg.DateCompactRelativeThresholdDays);
        const bool isDescriptionField =
            !column.FieldId.empty() && (column.FieldId.find("description") != std::string::npos ||
                                        column.FieldId.find("Description") != std::string::npos);
        // Description tooltip needs the raw markdown source so the markdown
        // preview pipeline can parse it; date-like fields show the full ISO.
        const std::string* saveTip = (column.IsDateLike || isDescriptionField) ? &currentValue : nullptr;
        RenderClippedFieldText(saveDisplay, valueAvailWidth, d.cfg.EnableFieldOverflowTooltips, true, saveTip,
                               isDescriptionField, &column.FieldId);
        ImGui::EndGroup();
        cellGroupMin = ImGui::GetItemRectMin();
        cellGroupMax = ImGui::GetItemRectMax();
        DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                     ticket.GetFieldRichValue(column.FieldId), &app, &d, readOnlyMode, &ticket);
    } else if (column.FieldId == "comments") {
        // issue-comments PR-A — backend-agnostic comments cell. Comments are read-only (editmeta
        // marks the field non-editable), so this bypasses the field-edit path entirely. The count
        // comes from the cached fieldValues["comments"] ("0","3",… for GitHub) — NO per-row network.
        // Plane (PR-C) leaves the value empty, so the icon-only branch must handle empty/non-numeric.
        ImGui::BeginGroup();
        bool isNumber = !currentValue.empty();
        for (size_t ci = 0; ci < currentValue.size(); ++ci) {
            if (currentValue[ci] < '0' || currentValue[ci] > '9') {
                isNumber = false;
                break;
            }
        }
        // 0xF0 0x9F 0x92 0xAC == U+1F4AC SPEECH BALLOON (💬). Built as a string literal so the
        // selectable label is unique per cell (id includes the issue id) without a per-row alloc on
        // the common path beyond the label itself.
        const char* kBalloon = "\xf0\x9f\x92\xac";
        std::string label = isNumber ? (std::string(kBalloon) + " " + currentValue) : std::string(kBalloon);
        label += "##cmt_" + ticket.id;
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(valueAvailWidth, 0.0f))) {
            OpenCommentsModal(app, ticket.id);
        }
        if (ImGui::IsItemHovered()) {
            // issue-comments fix (#1291) — match the retired Jira "Comment" field: on hover show the
            // full comment text wrapped. The Jira mapper resolves it into fieldValues["comment"]
            // (catalog-independent, so it survives the legacy field's removal from the picker); read it
            // zero-copy from the cache — NO network on hover. Backends without a comment blob
            // (GitHub/Plane) fall back to the cheap localized static hint.
            const std::string& commentBlob = ticket.GetFieldValueRef("comment");
            if (!commentBlob.empty()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                ImGui::TextUnformatted(commentBlob.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            } else {
                ImGui::SetTooltip("%s", SmatchetLocalization::T("comments.cell_tooltip", "View / post comments"));
            }
        }
        ImGui::EndGroup();
        cellGroupMin = ImGui::GetItemRectMin();
        cellGroupMax = ImGui::GetItemRectMax();
        DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                     ticket.GetFieldRichValue(column.FieldId), &app, &d, readOnlyMode, &ticket);
    } else {
        const bool allowEditsForCell = !readOnlyMode && column.NeedsAllowEditsCheck &&
                                       app.CanEditFieldForIssue(ticket.id, column.FieldId, fieldMeta);
        ImGui::BeginGroup();
        TicketFieldEditor::RenderFieldCell(app, ticket, column, colIndex, fieldMeta, currentValue, valueAvailWidth,
                                           d.cfg.EnableFieldOverflowTooltips, allowEditsForCell, ctx.pane.gridState,
                                           pendingEdits, d.trackerGridAsync, d.cfg.DateFormatOption,
                                           d.cfg.DateCompactRelativeThresholdDays, d.cfg.SingleClickToEditGridCells);
        ImGui::EndGroup();
        cellGroupMin = ImGui::GetItemRectMin();
        cellGroupMax = ImGui::GetItemRectMax();
        DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                     ticket.GetFieldRichValue(column.FieldId), &app, &d, readOnlyMode, &ticket);
    }

    if (showBadge) {
        const ImVec2 textSize = ImGui::CalcTextSize(badgeText.c_str());
        const float badgeX = (std::max)(ImGui::GetCursorScreenPos().x, cellRightX - textSize.x);
        ImGui::SetCursorScreenPos(ImVec2(badgeX, cellStartY));
        ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
        ImGui::TextUnformatted(badgeText.c_str());
        ImGui::PopStyleColor();
        if (!badgeTooltip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", badgeTooltip.c_str());
        }
    }

    handleActiveProjectCellRectSel(ctx, clippedRow, colIndex, cellOriginX, cellWidth, cellGroupMin.y, cellGroupMax.y,
                                   false, ticket.id, false);
}

// Google-Sheets-style per-cell selection gesture (formerly the handleCellRectSel lambda).
// Cell geometry arrives as floats (cellOriginX / cellWidth / groupMinY / groupMaxY) so the
// declaration needs no imgui include in the header.
void SmatchetUI::handleActiveProjectCellRectSel(ActiveProjectDrawCtx& ctx, int rowIdx, int colIdx, float cellOriginX,
                                                float cellWidth, float groupMinY, float groupMaxY, bool isIdCol,
                                                const std::string& issueId, bool activeIssueWasThisRow) {
    GridPane& pane = ctx.pane;
    bool& rectCellClickedThisFrame = ctx.rectCellClickedThisFrame;
    const std::uint64_t gridSortSig = ctx.gridSortSig;

    const bool shiftHeld = ImGuiEffectiveKeyShift();
    const bool ctrlHeld = ImGuiEffectiveKeyCtrl();
    const ImVec2 hitMin(cellOriginX, groupMinY);
    const ImVec2 hitMax(cellOriginX + cellWidth, groupMaxY);
    auto& sel = pane.gridState.RectSel;
    const bool hovering = ImGui::IsMouseHoveringRect(hitMin, hitMax, false);
    const bool mouseClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    if (hovering && mouseClick && sel.Contains(rowIdx, colIdx)) {
        rectCellClickedThisFrame = true;
    }

    if (hovering && mouseClick) {
        if (!issueId.empty()) {
            pane.gridState.SetActiveIssue(issueId);
        }
        if (isIdCol) {
            // Row-header gestures on the key column.
            if (shiftHeld) {
                const int anchorRow = (sel.PrimaryRow >= 0) ? sel.PrimaryRow : rowIdx;
                std::set<int> baseBefore = sel.Rows;
                const int lo = (anchorRow < rowIdx) ? anchorRow : rowIdx;
                const int hi = (anchorRow > rowIdx) ? anchorRow : rowIdx;
                for (int i = lo; i <= hi; ++i)
                    sel.Rows.insert(i);
                sel.PrimaryRow = anchorRow;
                sel.DragRowMode = true;
                sel.DragStartRow = anchorRow;
                sel.DragBaseRows = std::move(baseBefore);
                sel.Active = false;
                sel.Dragging = true;
            } else if (ctrlHeld) {
                auto it = sel.Rows.find(rowIdx);
                const bool wasSelected = (it != sel.Rows.end());
                if (!wasSelected) {
                    sel.Rows.insert(rowIdx);
                } else {
                    sel.Rows.erase(it);
                    if (activeIssueWasThisRow) {
                        pane.gridState.ActiveIssueId.clear();
                    }
                }
                sel.PrimaryRow = rowIdx;
                sel.DragRowMode = false;
                sel.DragStartRow = -1;
                sel.DragBaseRows.clear();
                sel.Active = false;
                sel.Dragging = false;
            } else {
                sel.Rows.clear();
                sel.Rows.insert(rowIdx);
                sel.PrimaryRow = rowIdx;
                sel.DragRowMode = true;
                sel.DragStartRow = rowIdx;
                sel.DragBaseRows.clear();
                sel.Active = false;
                sel.Dragging = true;
            }
            sel.SortSignature = gridSortSig;
            rectCellClickedThisFrame = true;
        } else {
            sel.ClearAll();
            sel.SortSignature = gridSortSig;
            rectCellClickedThisFrame = true;
        }
    } else if (sel.Dragging && hovering && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Drag update: extend the active gesture while the mouse is held.
        if (sel.DragRowMode) {
            const int lo = (sel.DragStartRow < rowIdx) ? sel.DragStartRow : rowIdx;
            const int hi = (sel.DragStartRow > rowIdx) ? sel.DragStartRow : rowIdx;
            sel.Rows = sel.DragBaseRows;
            for (int i = lo; i <= hi; ++i)
                sel.Rows.insert(i);
        } else {
            sel.ExtentRow = rowIdx;
            sel.ExtentCol = colIdx;
        }
    }

    if (sel.Contains(rowIdx, colIdx)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(hitMin, hitMax, IM_COL32(66, 135, 245, 60));
        dl->AddRect(hitMin, hitMax, IM_COL32(66, 135, 245, 180));
    }
}

void SmatchetUI::drawActiveProjectGridNewIssue(ActiveProjectDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    const bool readOnlyMode = ctx.readOnlyMode;

    // The new-issue draft is session-level state (one draft) — focused pane only.
    if (!readOnlyMode && ctx.pane.focused) {
        SMATCHET_UI_PERF_SCOPE("activeProject:grid.newIssue");
        const CachedTicket* lastVisibleTicket = nullptr;
        if (!tickets.empty()) {
            size_t lastIndex = tickets.size() - 1;
            if (!ctx.pane.filteredIndices.empty()) {
                lastIndex = ctx.pane.filteredIndices.back();
            }
            if (lastIndex < tickets.size()) {
                lastVisibleTicket = &tickets[lastIndex];
            }
        }
        RenderNewIssueDraftRow(app, d, columns, d.cfg, lastVisibleTicket);
    }
}

void SmatchetUI::drawActiveProjectGridPost(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    ViewDefinition* activeViewForGrid = ctx.activeViewForGrid;

    SMATCHET_UI_PERF_SCOPE("activeProject:grid.post");
    RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGui::GetCurrentTable(), d, ctx.pane);

    // Capture column widths and sort specs into the active view IN MEMORY so the
    // grid renders the user's drag/sort immediately. The unsaved-layout strip gates
    // these changes behind Save / Discard; the snapshot on first mutation lets Discard
    // revert. Focused pane only — these mutate the session view-edit state, and
    // resizing a non-focused pane's table first click-focuses it anyway.
    if (activeViewForGrid && ctx.pane.focused) {
        ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
        if (mutableActive) {
            bool metaChanged = false;
            ImGuiTable* table = ImGui::GetCurrentTable();
            if (table) {
                for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
                    const std::string& key = columns[static_cast<size_t>(i)].Key;
                    const float width = (i < table->ColumnsCount) ? table->Columns[i].WidthGiven : 0.0f;
                    const auto oldIt = mutableActive->ColumnWidths.find(key);
                    const float oldWidth = (oldIt == mutableActive->ColumnWidths.end()) ? 0.0f : oldIt->second;
                    if (std::abs(oldWidth - width) > 0.5f) {
                        if (!metaChanged) {
                            SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                        }
                        mutableActive->ColumnWidths[key] = width;
                        metaChanged = true;
                    }
                }
            }
            // Re-fetch sort specs from the table right before persisting so we use current state.
            ImGuiTableSortSpecs* currentSortSpecs = ImGui::TableGetSortSpecs();
            if (currentSortSpecs && currentSortSpecs->SpecsCount > 0 && currentSortSpecs->Specs != nullptr) {
                std::vector<ViewSortSpec> newSortSpecs;
                for (int s = 0; s < currentSortSpecs->SpecsCount; ++s) {
                    const int colIndex = currentSortSpecs->Specs[s].ColumnIndex;
                    if (colIndex >= 0 && colIndex < static_cast<int>(columns.size()) &&
                        IsPersistableSortDirection(currentSortSpecs->Specs[s].SortDirection)) {
                        ViewSortSpec vs;
                        vs.ColumnKey = columns[static_cast<size_t>(colIndex)].Key;
                        vs.Direction = static_cast<int>(currentSortSpecs->Specs[s].SortDirection);
                        newSortSpecs.push_back(vs);
                    }
                }
                if (newSortSpecs != mutableActive->SortSpecs) {
                    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                    mutableActive->SortSpecs = std::move(newSortSpecs);
                    metaChanged = true;
                }
            } else {
                if (!mutableActive->SortSpecs.empty()) {
                    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                    mutableActive->SortSpecs.clear();
                    metaChanged = true;
                }
            }
            if (metaChanged) {
                // Bump revision so the grid frame context rebuilds with new widths/sort
                // next frame. Surface as dirty so the user can Save / Discard. No
                // debounced disk write here — explicit Save commits everything.
                ViewState.BumpRevision();
                d.viewsDirty = true;
            }

            // Capture user-driven column reorder (drag the header) into the
            // editing buffer; mark the view dirty so the unsaved-layout strip
            // appears. Don't autosave: column order changes are destructive,
            // unlike width/sort which the user can revert with another drag.
            ImGuiTable* tableForOrder = ImGui::GetCurrentTable();
            if (tableForOrder && tableForOrder->ColumnsCount > 0) {
                std::vector<std::string> visualOrder;
                visualOrder.reserve(columns.size());
                for (int v = 0; v < tableForOrder->ColumnsCount; ++v) {
                    // DisplayOrderToIndex is an ImSpan<short> sized to ColumnsCount.
                    if (v >= tableForOrder->DisplayOrderToIndex.size()) {
                        break;
                    }
                    const int logical = tableForOrder->DisplayOrderToIndex[v];
                    if (logical < 0 || logical >= static_cast<int>(columns.size())) {
                        continue;
                    }
                    visualOrder.push_back(columns[static_cast<size_t>(logical)].Key);
                }
                if (!visualOrder.empty() && visualOrder != mutableActive->ColumnOrder) {
                    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                    d.editingColumnOrder = visualOrder;
                    d.viewsDirty = true;
                }
            }
        }
    }
}

void SmatchetUI::drawActiveProjectGridRectSelKeys(ActiveProjectDrawCtx& ctx) {
    GridPane& pane = ctx.pane;
    const std::vector<CachedTicket>& tickets = ctx.tickets;
    const std::vector<TicketGridColumn>& columns = ctx.columns;
    const TrackerFieldCatalogIndex& catalogIndex = ctx.catalogIndex;
    const bool rectCellClickedThisFrame = ctx.rectCellClickedThisFrame;
    const bool ticketGridLeftClickInsideTableHit = ctx.ticketGridLeftClickInsideTableHit;
    const std::uint64_t gridSortSig = ctx.gridSortSig;

    if (!columns.empty()) {
        SMATCHET_UI_PERF_SCOPE("activeProject:grid.rectSel.keys");
        auto& sel = pane.gridState.RectSel;
        if (sel.Dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            sel.Dragging = false;
            sel.DragRowMode = false;
            sel.DragBaseRows.clear();
        }
        const ImGuiIO& io = ImGui::GetIO();
        const bool effShift = ImGuiEffectiveKeyShift();
        const bool effCtrl = ImGuiEffectiveKeyCtrl();
        // Strict hover: omit ImGuiHoveredFlags_AllowWhenBlockedByActiveItem so a
        // click into a text input in another docked panel (e.g. the AI Assistant
        // side panel input) doesn't satisfy this branch and clear the grid
        // selection. The clear contract is "user clicked on empty space inside
        // the Active Project window" — clicking another window is not that.
        const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        // Click outside any currently-selected cell (without Shift/Ctrl) clears
        // the entire selection. Ctrl preserves selection for toggling; Shift
        // preserves it for range extension. Skip when text input elsewhere is
        // claiming the click — typing into the AI assistant / palette / filter
        // bar must not nuke the grid selection.
        if ((sel.HasAnySelection() || !pane.gridState.ActiveIssueId.empty()) && !effShift && !effCtrl &&
            windowHovered && !io.WantTextInput && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !rectCellClickedThisFrame && !ticketGridLeftClickInsideTableHit) {
            sel.ClearAll();
            pane.gridState.ActiveIssueId.clear();
        }
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // Ctrl+A selects every row. Outside the HasAnySelection() guard below
        // (select-all needs no prior selection) and gated on !effShift so
        // Ctrl+Shift+A (Toggle Assistant) doesn't trigger it. !io.WantTextInput
        // keeps a text field's own select-all intact.
        if (windowFocused && !io.WantTextInput && effCtrl && !effShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            GridSelectAllRows(pane, tickets);
        }
        if (windowFocused && sel.HasAnySelection()) {
            if (!io.WantTextInput && effCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                CopyGridRectAsTsv(tickets, pane.filteredIndices, columns, catalogIndex, sel);
            }
            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                sel.ClearAll();
            }
        }
        // Shift+Space: promote the row containing the active cell to a whole-
        // row selection (in addition to whatever is already selected).
        if (windowFocused && !io.WantTextInput && effShift && !effCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            PromoteActiveRowToSelection(pane, sel, tickets, gridSortSig);
        }
    }
}
