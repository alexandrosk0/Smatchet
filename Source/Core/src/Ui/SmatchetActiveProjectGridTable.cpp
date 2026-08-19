#include "SmatchetUI.h"
#include "SmatchetActiveProjectGridUi_Internal.h"
#include "SmatchetGridPaneWindows.h" // detail::PaneViewSelfRepairAllowed (HIGH-1) + ChoosePaneColumnsSource
#include "SmatchetGridUiSupport.h"
#include "SmatchetViewsDashboardUi_detail.h"

// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=this sibling TU is a byte-identical god-file-split carve-out of SmatchetActiveProjectGridUi.cpp and needs the full AppController to drive ctx.app.* on the grid table/cell paths — same shape as the AnnotateAnalysisUi_Modals/_Window split; owner=orchestrator; revisit=when the grid draw context is narrowed to an interface)
#include "AppController.h"
#include "ConfigManager.h"
#include "GridColumnOrderPure.h"
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

// ImGui rebuilds a live table's column storage whenever the column COUNT changes (the
// "preserve widths across a spec change" path in BeginTableEx) and seeds
// DisplayOrderToIndex[n] from Columns[n].DisplayOrder -- the inverse of the order->index
// mapping that array actually holds. That seed is only correct while the display order is
// still identity. The corrective rebuild (fix the orders, then scatter
// DisplayOrderToIndex[Columns[n].DisplayOrder] = n) lives at the tail of TableLoadSettings,
// which returns on its first line for a NoSavedSettings table -- and TicketGrid is one. So
// adding or removing a grid field while a header drag-reorder is in effect leaves the map
// inverted: columns render in the wrong visual slots. Worse, the next drag walks that bad
// map to shift the neighbours it steps over, which can land two columns on one DisplayOrder
// and leave a stale slot behind -- the visual-order writeback then persists the same key
// twice into ColumnOrder.
//
// Detect the desync and reset both arrays to identity. Identity is the right answer here:
// columns[] is already built in the view's saved order, so the table's own permutation only
// ever carries a drag the user has not committed yet, and a consistent map (fresh table, or
// post-drag once ImGui rescatters) always passes this check untouched.
static void RepairDesyncedTableDisplayOrder(ImGuiTable* table) {
    if (table == nullptr || table->ColumnsCount <= 0) {
        return;
    }
    if (table->ColumnsCount > table->DisplayOrderToIndex.size() || table->ColumnsCount > table->Columns.size()) {
        return;
    }
    const int mismatch = GridFirstDisplayOrderMismatch(
        table->ColumnsCount, [table](int n) { return static_cast<int>(table->Columns[n].DisplayOrder); },
        [table](int order) { return static_cast<int>(table->DisplayOrderToIndex[order]); });
    if (mismatch < 0) {
        return;
    }
    for (int r = 0; r < table->ColumnsCount; ++r) {
        table->Columns[r].DisplayOrder = static_cast<ImGuiTableColumnIdx>(r);
        table->DisplayOrderToIndex[r] = static_cast<ImGuiTableColumnIdx>(r);
    }
}

// Capture a user-driven column reorder (drag the header) into the editing buffer and mark the
// view dirty so the unsaved-layout strip appears. Deliberately not autosaved: a column-order
// change is destructive, unlike width/sort which the user can revert with another drag.
static void CaptureHeaderDragColumnOrder(UiDrawSession& d, const std::vector<TicketGridColumn>& columns,
                                         ViewDefinition& activeView) {
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (table == nullptr || table->ColumnsCount <= 0) {
        return;
    }
    std::vector<std::string> visualOrder;
    visualOrder.reserve(columns.size());
    for (int v = 0; v < table->ColumnsCount; ++v) {
        // The order->index map is an ImSpan<short> sized to ColumnsCount.
        if (v >= table->DisplayOrderToIndex.size()) {
            break;
        }
        const int logical = table->DisplayOrderToIndex[v];
        if (logical < 0 || logical >= static_cast<int>(columns.size())) {
            continue;
        }
        visualOrder.push_back(columns[static_cast<size_t>(logical)].Key);
    }
    // Persist only a true permutation of the column set: an order that comes out short, or that
    // repeats a key, means the display-order map was inconsistent this frame. Writing it would
    // bake a duplicate key into ColumnOrder, and the load-side dedupe then drops a column back to
    // the end of the grid.
    if (!GridVisualColumnOrderIsPermutation(visualOrder, columns.size())) {
        return;
    }
    // Compare against the editing buffer too: a saved ColumnOrder holding a stale key that the
    // column set no longer carries never converges on the visual order, so a saved-only compare
    // would re-capture and re-dirty the view every frame until the user hits Save.
    if (visualOrder == activeView.ColumnOrder || visualOrder == d.editingColumnOrder) {
        return;
    }
    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, activeView);
    d.editingColumnOrder = visualOrder;
    d.viewsDirty = true;
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

namespace {

// ---- Grid body states (UX critique H1/H4) -------------------------------------------
// The primary workspace used to render a bare empty table whether a sync was in flight,
// the query matched nothing, or the backend was down — indistinguishable to the user.
// These helpers give each situation an explicit face. The welcome/loading/error states
// REPLACE the table (no rows are possible there — the error state also forces read-only,
// so the inline new-issue row is moot); the zero-results state draws as a strip ABOVE the
// table so the headers and the inline "+ new issue" row stay usable.

// Begin a horizontally-centered fixed-width child for a state block. Pair with EndChild.
bool BeginCenteredGridStateBlock(const char* id, float blockW) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > blockW) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - blockW) * 0.5f);
    }
    // Vertical inset so the block sits in the upper third of the body, not glued to the top.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (std::max)(0.0f, ImGui::GetContentRegionAvail().y * 0.18f));
    return ImGui::BeginChild(id, ImVec2((std::min)(blockW, avail), 0.0f), false, ImGuiWindowFlags_NoScrollbar);
}

// H4 — first-run orientation: the tracker gate locks the app until a backend probe
// succeeds, but the locked state had no face. One line of what Smatchet is, the backend
// choices with token deep-links, and the path forward (Preferences > Tracker).
void DrawGridWelcomeState(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (!BeginCenteredGridStateBlock("##GridWelcomeState", 560.0f)) {
        ImGui::EndChild();
        return;
    }
    ImGui::TextUnformatted(SmatchetLocalization::T("grid.welcome.title", "Welcome to Smatchet"));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s", SmatchetLocalization::T("grid.welcome.blurb",
                                      "Smatchet is a fast desktop grid for your issue tracker. Connect a backend and "
                                      "your issues appear here."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", SmatchetLocalization::T("grid.welcome.step",
                                                     "To get started, pick a backend in Preferences > Tracker and "
                                                     "paste its API credentials:"));
    ImGui::Spacing();
    struct BackendHint {
        const char* line;
        const char* linkLabel;
        const char* url;
    };
    const BackendHint kHints[] = {
        {"Jira — site URL, account email, and an API token", "Get a Jira API token",
         "https://id.atlassian.com/manage-profile/security/api-tokens"},
        {"GitHub — a personal access token with repo scope", "Get a GitHub token",
         "https://github.com/settings/tokens"},
        {"Plane — workspace URL and a personal API token", "Get a Plane API token",
         "https://docs.plane.so/api-reference/introduction"},
        {"Linear — a personal API key", "Get a Linear API key", "https://linear.app/settings/api"},
    };
    for (int i = 0; i < static_cast<int>(sizeof(kHints) / sizeof(kHints[0])); ++i) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", SmatchetLocalization::TranslateSource(kHints[i].line));
        ImGui::SameLine();
        ImGui::PushID(i);
        if (ImGui::SmallButton(SmatchetLocalization::TranslateSource(kHints[i].linkLabel))) {
            ctx.app.OpenUrl(kHints[i].url);
        }
        // P2-L5: external-link cue — say where the browser is about to go.
        ImGui::SetItemTooltip("Opens in your browser: %s", kHints[i].url);
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button(SmatchetLocalization::T("grid.welcome.open_prefs", "Open Preferences"))) {
        d.showPreferences = true;
        d.requestPreferencesFocus = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SmatchetLocalization::T("grid.welcome.gate_note",
                                                      "The rest of the app unlocks once a connection succeeds."));
    ImGui::EndChild();
}

// H1 (loading) — a sync or catalog fetch is in flight and no rows are cached yet.
void DrawGridLoadingState() {
    if (!BeginCenteredGridStateBlock("##GridLoadingState", 320.0f)) {
        ImGui::EndChild();
        return;
    }
    const int dots = static_cast<int>(::ImGui::GetTime() * 2.0) % 4;
    char label[48];
    std::snprintf(label, sizeof(label), "%s%.*s", SmatchetLocalization::T("grid.state.loading", "Loading issues"), dots,
                  "...");
    ImGui::TextUnformatted(label);
    ImGui::TextDisabled(
        "%s", SmatchetLocalization::T("grid.state.loading.detail", "Fetching this view's issues from the backend."));
    ImGui::EndChild();
}

// H1 (error) — the backend errored and nothing is cached: surface the connectivity
// reason (same text as the status bar / banner) plus an explicit Retry.
void DrawGridErrorState(ActiveProjectDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    if (!BeginCenteredGridStateBlock("##GridErrorState", 480.0f)) {
        ImGui::EndChild();
        return;
    }
    ImGui::TextColored(SmatchetTheme::Colors::StatusBlocked, "%s",
                       SmatchetLocalization::T("grid.state.error", "Couldn't load issues"));
    ImGui::Spacing();
    if (!ctx.trackerBanner.Message.empty()) {
        ImGui::TextWrapped("%s", ctx.trackerBanner.Message.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button(SmatchetLocalization::T("grid.state.error.retry", "Retry"))) {
        d.triggerCatalogRefetch = true;
        d.connectivityRecoveryTicketResyncPending = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("grid.state.error.open_prefs", "Open Preferences"))) {
        d.showPreferences = true;
        d.requestPreferencesFocus = true;
    }
    ImGui::EndChild();
}

// H1 (zero results) — connected and loaded, but nothing to show: either the view's query
// matched nothing (`viewIsEmpty`), or the quick filter hides every loaded row. Drawn as a
// one-line strip ABOVE the table (headers + inline new-issue row stay usable below). The
// cause is passed explicitly — a stale filter buffer carried over from a prior view must
// not relabel a genuinely-empty view as a filter miss (Clear filter would be a no-op).
void DrawGridZeroResultsStrip(ActiveProjectDrawCtx& ctx, bool viewIsEmpty) {
    UiDrawSession& d = ctx.d;
    GridPane& pane = ctx.pane;
    const bool filterActive = !viewIsEmpty && pane.gridFilterBuf[0] != '\0';
    ImGui::AlignTextToFramePadding();
    if (filterActive) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("grid.state.filter_no_match", "No issues match the filter."));
        ImGui::SameLine();
        if (ImGui::SmallButton(SmatchetLocalization::T("grid.state.clear_filter", "Clear filter"))) {
            pane.gridFilterBuf[0] = '\0';
        }
    } else {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("grid.state.no_results", "No issues match this view."));
        ImGui::SameLine();
        if (ImGui::SmallButton(SmatchetLocalization::T("grid.state.edit_view", "Edit view..."))) {
            d.showViewsDashboard = true;
            d.requestViewsDashboardFocus = true;
        }
    }
}

} // namespace

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

    // Explicit grid body states (UX critique H1/H4): never render a bare empty table.
    // Welcome / loading / error REPLACE the table; zero-results draws a strip above it.
    // The replacing states skip drawActiveProjectGridRows — the frame-by-frame pruner of
    // stale filteredIndices (indices into a previous, larger snapshot) and the dependent
    // rect selection — so prune here too, or chrome reading them (menu Copy enablement,
    // Ctrl+C in the rect-sel key handler) sees ghosts of the previous snapshot's rows.
    if (!d.cfg.BackendHasBeenReachable) {
        ctx.pane.filteredIndices.clear();
        ctx.pane.gridState.RectSel.ClearAll();
        DrawGridWelcomeState(ctx);
        return;
    }
    if (ctx.tickets.empty()) {
        // P2-M9: the session-scoped flags below only track the INITIAL sync; every pane
        // runs its own live sync, so a pane mid-first-fetch (sync live, no snapshot rows
        // published yet) must read as loading too — not "No issues match this view."
        const bool paneFirstFetchLoading = ctx.pane.snapshotRevision == 0 && ctx.app.IsPaneSyncLive(ctx.pane.id);
        const bool syncLoading = d.initialTicketSyncLoading || d.fieldCatalogLoading ||
                                 d.connectivityRecoveryTicketFetchLoading || !d.initialTicketSyncStarted ||
                                 paneFirstFetchLoading;
        if (syncLoading || ctx.trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
            ctx.pane.filteredIndices.clear();
            ctx.pane.gridState.RectSel.ClearAll();
            if (syncLoading) {
                DrawGridLoadingState();
            } else {
                DrawGridErrorState(ctx);
            }
            return;
        }
        DrawGridZeroResultsStrip(ctx, /*viewIsEmpty=*/true);
    } else if (ctx.pane.filteredIndices.empty() && ctx.pane.gridFilterBuf[0] != '\0') {
        // Loaded rows exist but the quick filter hides them all (filteredIndices lags one
        // frame behind the projection rebuild inside the table — fine for a hint strip).
        DrawGridZeroResultsStrip(ctx, /*viewIsEmpty=*/false);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));
    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        // Repair an inverted display-order map before anything reads it (see
        // RepairDesyncedTableDisplayOrder): adding or removing a grid field changes the
        // column count, which is exactly when ImGui can leave the map inconsistent for a
        // NoSavedSettings table.
        RepairDesyncedTableDisplayOrder(ImGui::GetCurrentTable());
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
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the ActiveProjectDrawCtx unpack prologue (AppController& app = ctx.app; ... = ctx.*) is shared by every grid section helper by construction; the god-file-split only moved these helpers into separate TUs, so a pre-existing intra-file pattern reads as a cross-file clone; owner=orchestrator; revisit=when the draw context exposes typed accessors)
    // clang-format on
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

    // ticket-change-monitor: one-shot scroll to a focus-requested ticket (a Notification Center row
    // click → FocusTicketInGrid latches d.pendingFocusIssueId). Resolve the target's ordered row
    // index and pixel-scroll to it — clipper-safe, mirroring the scenario SetScrollY path — then
    // clear the latch. Focused pane only, so a background pane showing the same id doesn't grab it.
    if (!ctx.d.pendingFocusIssueId.empty() && pane.focused) {
        const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        for (size_t r = 0; r < indicesToUse.size(); ++r) {
            const size_t ti = indicesToUse[r];
            if (ti < tickets.size() && tickets[ti].id == ctx.d.pendingFocusIssueId) {
                ImGui::SetScrollY(static_cast<float>(r) * rowH);
                ctx.d.pendingFocusIssueId.clear();
                break;
            }
        }
    }

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
    //
    // ALSO strict view ownership, on both halves: `columns` was built for the RENDERED
    // view, but every write below lands in ViewState.GetActiveViewMutable() — a
    // different object whenever resolvePaneView hands back something that is not this
    // pane's own view (a cross-backend pane whose self-repair is refused renders the
    // other backend's active view, and a pane holding its OWN stored view is not
    // necessarily the globally-active one). Without the Id equality a width drag or a
    // header reorder in such a pane bakes one view's column keys into another view's
    // ColumnWidths / SortSpecs / ColumnOrder. Same class as the HIGH-1 sort-mirror
    // guard above, and the same argument makes Id equality sufficient: resolvePaneView's
    // self-repair already ran this draw, so a pane still mismatching here is exactly a
    // repair-refused one.
    const bool paneOwnsRenderedView = activeViewForGrid && activeViewForGrid->Id == ctx.pane.viewId;
    if (paneOwnsRenderedView && ctx.pane.focused) {
        ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
        if (mutableActive && mutableActive->Id == ctx.pane.viewId) {
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

            // Capture a user-driven column reorder (drag the header) into the editing
            // buffer; see CaptureHeaderDragColumnOrder.
            CaptureHeaderDragColumnOrder(d, columns, *mutableActive);
        }
    }
}
