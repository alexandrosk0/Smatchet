#include "SmatchetUI.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetViewsDashboardUi_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "SmatchetInputModifierBridge.h"
#include "SmatchetUiSession.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
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

/** When vertically at top/bottom (or no vertical scroll), map mouse wheel to horizontal scroll; first N wheel ticks at
 * each end ignored (configured by GridEndWheelSwallowsBeforeHorizontal). */
static void RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGuiTable* table, UiDrawSession& d) {
    const int endWheelSwallowsBeforeHorizontal = (std::max)(0, d.cfg.GridEndWheelSwallowsBeforeHorizontal);

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
        d.gridBottomHorizontalWheelSwallowsRemaining = 0;
    }
    if (!atTop) {
        d.gridTopHorizontalWheelSwallowsRemaining = 0;
    }

    bool allowRoute = false;
    if (noVerticalScroll) {
        allowRoute = true;
    } else if (atBottom) {
        if (d.gridBottomHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++d.gridBottomHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    } else if (atTop) {
        if (d.gridTopHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++d.gridTopHorizontalWheelSwallowsRemaining;
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

} // namespace

void SmatchetUI::drawActiveProjectWindow(AppController& app, UiDrawSession& d) {
    const bool wantFocus = d.requestActiveProjectFocus;
    prepareTopLevelWindow(d, "active", 900.0f, 620.0f, wantFocus);
    if (!ImGui::Begin("Smatchet - Active Project", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "active", 420.0f, 300.0f);
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestActiveProjectFocus = false;
    }
    const TrackerConnectivityBannerForUi TrackerBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
    MaybeToastTrackerConnectivityBanner(app, d, TrackerBanner);
    const bool readOnlyMode =
        d.cfg.ReadOnlyMode || (TrackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error);

    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    ViewDefinition* activeViewForGrid = ViewState.GetActiveViewMutable();
    const TrackerFieldCatalogIndex& catalogIndex = *gridFrameCtx_.catalogIndex;
    const std::vector<TicketGridColumn>& columns = gridFrameCtx_.columns;

    // Tab bar: Grid is always shown. Annotate is only rendered while annotateTabVisible.
    // When the Annotate tab is hidden, force activeGridTab back to Grid so stale state
    // doesn't keep the panel "open" for background services.
    if (!d.annotateTabVisible && d.activeGridTab != 0) {
        d.activeGridTab = 0;
    }
    blameAnalysisUi_.SetBlamePanelOpen(d.annotateTabVisible && d.activeGridTab == 1);
    blameAnalysisUi_.ServiceBackground();
    if (d.annotateTabVisible) {
        if (ImGui::BeginTabBar("##active_project_tabs")) {
            const bool forceSwitch = d.activeGridTabForcePending;
            d.activeGridTabForcePending = false;
            const ImGuiTabItemFlags gridFlags =
                (d.activeGridTab == 0 && forceSwitch) ? ImGuiTabItemFlags_SetSelected : 0;
            const ImGuiTabItemFlags annotateFlags =
                (d.activeGridTab == 1 && forceSwitch) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Grid", nullptr, gridFlags)) {
                d.activeGridTab = 0;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Annotate", nullptr, annotateFlags)) {
                d.activeGridTab = 1;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        d.activeGridTabForcePending = false;
    }

    if (d.annotateTabVisible && d.activeGridTab == 1) {
        bool wantClose = false;
        blameAnalysisUi_.DrawContent(app, &wantClose, d.gridState.ActiveIssueId);
        if (wantClose) {
            d.annotateTabVisible = false;
            d.activeGridTab = 0;
        }
        ImGui::End();
        return;
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:header");
        DrawGridHeaderToolbar(app, d, activeViewForGrid, columns, tickets, readOnlyMode, ViewState, TrackerBanner);
    }

    const bool viewChanged = activeViewForGrid && (activeViewForGrid->Id != d.lastGridActiveViewId);
    if (viewChanged && activeViewForGrid) {
        d.lastGridActiveViewId = activeViewForGrid->Id;
        d.viewSortDirty = false;
        // Active view switched — abandon unsaved edits that belonged to the old
        // view (no confirmation in the grid path; Views editor has its own modal).
        d.viewsDirty = false;
        d.viewsHasOriginalSnapshot = false;
    }
    if (!activeViewForGrid) {
        d.lastGridActiveViewId.clear();
        d.viewSortDirty = false;
        d.viewsDirty = false;
        d.viewsHasOriginalSnapshot = false;
    }

    const std::string gridContextSignature = BuildGridContextSignature(activeViewForGrid, d.cfg.JqlQuery);
    const bool gridContextChanged =
        !d.lastGridContextSignature.empty() && gridContextSignature != d.lastGridContextSignature;
    if (gridContextChanged) {
        CancelUnfinishedNewIssueForGridChange(d);
    }
    d.lastGridContextSignature = gridContextSignature;

    const bool gridSortEnvironmentChanged = viewChanged || gridContextChanged;

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
            updated.Jql = d.viewJqlBuf[0] ? std::string(d.viewJqlBuf) : activeViewForGrid->Jql;
            const std::vector<std::string> editedFields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
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
                SmatchetToastManager::Instance().Push("View saved", updated.Name, ToastType::Success, 1500);
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
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, restoreSource->Jql);
            const std::string fieldsCsv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(restoreSource->Fields);
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, fieldsCsv);
            d.viewsDirty = false;
            d.viewSortDirty = false;
            d.viewsHasOriginalSnapshot = false;
            d.pendingViewStateSave = false;
            ViewState.BumpRevision(); // force grid to redraw columns in the stored order
            SmatchetToastManager::Instance().Push("Reverted layout", restoreSource->Name, ToastType::Info, 1500);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Save-as-new modal: name input + Save / Cancel.
        if (ImGui::BeginPopupModal("Save view as new", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char s_newViewName[128] = {};
            if (ImGui::IsWindowAppearing()) {
                std::snprintf(s_newViewName, sizeof(s_newViewName), "%s (copy)", activeViewForGrid->Name.c_str());
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::TextUnformatted("New view name");
            ImGui::SetNextItemWidth(300.0f);
            const bool committed = ImGui::InputText("##NewViewName", s_newViewName, sizeof(s_newViewName),
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
                ViewDefinition created = *activeViewForGrid;
                created.Name = s_newViewName;
                created.Id.clear();
                if (!d.editingColumnOrder.empty()) {
                    created.ColumnOrder = d.editingColumnOrder;
                }
                const std::vector<std::string> editedFields =
                    SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
                if (!editedFields.empty()) {
                    created.Fields = editedFields;
                }
                if (d.viewJqlBuf[0]) {
                    created.Jql = d.viewJqlBuf;
                }
                ViewState.Create(created);
                const ViewDefinition* nowActive = ViewState.GetActiveView();
                if (nowActive) {
                    d.editingViewId = nowActive->Id;
                    d.lastSyncedColumnOrder = nowActive->ColumnOrder;
                    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewNameBuf, nowActive->Name);
                    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, nowActive->Jql);
                    const std::string fieldsCsv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(nowActive->Fields);
                    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, fieldsCsv);
                    d.editingColumnOrder = nowActive->ColumnOrder;
                    d.cfg.JqlQuery = nowActive->Jql;
                    d.cfg.SelectedFields = nowActive->Fields;
                    ConfigManager::Save(d.cfg);
                    SmatchetToastManager::Instance().Push("View created", nowActive->Name, ToastType::Success, 1500);
                }
                d.viewsDirty = false;
                d.viewsHasOriginalSnapshot = false;
                d.pendingViewStateSave = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    const bool drewOfflineSection = DrawUnifiedOfflineQueuesPanel(app, d);
    if (drewOfflineSection) {
        ImGui::Spacing();
        ImGui::SeparatorText("Ticket grid");
        ImGui::Spacing();
    }

    std::vector<PendingFieldEdit> pendingEdits;
    // Hoisted above the BeginTable scope so the post-EndTable block can read it
    // when deciding whether an out-of-selection click should clear the rect.
    bool rectCellClickedThisFrame = false;
    // Left click landed inside ticket table hitbox (OuterRect ∪ InnerClipRect)
    // but no cell set rectCellClickedThisFrame — blocks false outside-clear (H5).
    bool ticketGridLeftClickInsideTableHit = false;
    std::uint64_t gridSortSig = 0;
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                                       ImGuiTableFlags_SortTristate | ImGuiTableFlags_NoSavedSettings;

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));
    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        // Scenario-driven scroll: honor the target set by ScenarioRunner::Tick so automated
        // tests can drive the grid position without human input.
        if (d.scenarioScrollActive && d.scenarioScrollTarget >= 0) {
            ImGui::SetScrollY(static_cast<float>(d.scenarioScrollTarget));
        }
        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.setup");
            // Materialise column widths once (§3.1 item 56): avoids ColumnWidths.find per column per frame.
            std::vector<float> colWidths(columns.size());
            for (size_t ci = 0; ci < columns.size(); ++ci) {
                float w = (columns[ci].ColumnKind == TicketGridColumn::Kind::Id) ? 90.0f : 180.0f;
                if (activeViewForGrid) {
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
            if (activeViewForGrid) {
                ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
                const bool hasPersistedSort = !activeViewForGrid->SortSpecs.empty();
                const bool shouldApplyPersistedSort = specs && (gridSortEnvironmentChanged || d.forceApplySortSpecs);
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

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.sort");
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (gridSortEnvironmentChanged || d.forceApplySortSpecs) {
                d.cachedSortValid = false;
                d.forceApplySortSpecs = false;
            }
            if (sortSpecs && sortSpecs->SpecsDirty) {
                d.cachedSortValid = false;
                sortSpecs->SpecsDirty = false;

                // Sync header clicks back to View definition
                if (activeViewForGrid) {
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
                    bool changed = (newSpecs.size() != activeViewForGrid->SortSpecs.size());
                    if (!changed) {
                        for (size_t i = 0; i < newSpecs.size(); ++i) {
                            if (newSpecs[i].ColumnKey != activeViewForGrid->SortSpecs[i].ColumnKey ||
                                newSpecs[i].Direction != activeViewForGrid->SortSpecs[i].Direction) {
                                changed = true;
                                break;
                            }
                        }
                    }

                    if (changed) {
                        // Snapshot pre-change view so Discard can revert sort + widths
                        // + column order + buffers all at once.
                        SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *activeViewForGrid);
                        activeViewForGrid->SortSpecs = std::move(newSpecs);
                        d.viewSortDirty = true;
                        d.viewsDirty = true;
                    }
                }
            }
            const std::uint64_t activeTicketsRevision = app.GetActiveTicketsRevision();
            if (activeTicketsRevision != d.cachedSortTicketsRevision) {
                d.cachedSortValid = false;
            }
            const std::uint64_t catalogRevision = app.GetFieldCatalogRevision();
            if (catalogRevision != d.cachedSortCatalogRevision) {
                d.cachedSortValid = false;
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

            static thread_local char lastFilter[128]{};
            bool filterChanged = (std::strcmp(lastFilter, d.gridFilterBuf) != 0);
            if (filterChanged) {
                d.gridState.RectSel.ClearAll();
            }

            // Treat sort+filter as one cached projection with a dirty flag and refresh it at a bounded interval (500ms)
            // during streaming
            bool needsProjectionRefresh = false;
            if (!d.cachedSortValid || d.cachedSortFingerprint != fingerprint ||
                d.cachedSortedIndices.size() != tickets.size() ||
                activeTicketsRevision != d.cachedSortTicketsRevision ||
                catalogRevision != d.cachedSortCatalogRevision || filterChanged) {
                needsProjectionRefresh = true;
            }

            bool okToRefreshProjection = true;
            if (app.IsStreamingSyncActive() && needsProjectionRefresh) {
                // Debounce cheap reshuffles while tickets stream, but never delay a user-driven sort change:
                // header/persist fingerprint differs from last applied projection → apply immediately (fixes header
                // clicks feeling stuck while Sort By still worked due to slower interaction pacing).
                const bool sortKeyChanged = (fingerprint != d.cachedSortFingerprint);
                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - d.lastGridSortAt).count();
                if (!sortKeyChanged && elapsedMs < 500) {
                    okToRefreshProjection = false;
                }
            }

            if (needsProjectionRefresh && okToRefreshProjection) {
                d.lastGridSortAt = std::chrono::steady_clock::now();

                // 1. Run Sort Spec / Order Indices
                d.cachedSortedIndices.resize(tickets.size());
                for (size_t i = 0; i < tickets.size(); ++i) {
                    d.cachedSortedIndices[i] = i;
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
                    std::stable_sort(d.cachedSortedIndices.begin(), d.cachedSortedIndices.end(),
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
                                             const std::string aVal = tix[ia].GetFieldValue(sk.col->FieldId);
                                             const std::string bVal = tix[ib].GetFieldValue(sk.col->FieldId);
                                             const int cmp = CompareFieldValuesForSort(sk.col->FieldId, sk.fieldMeta,
                                                                                       aVal, bVal, sk.dir);
                                             if (cmp != 0)
                                                 return (cmp * sk.dir) < 0;
                                         }
                                         return ia < ib;
                                     });
                }

                d.cachedSortFingerprint = fingerprint;
                d.cachedSortValid = true;
                d.cachedSortTicketsRevision = activeTicketsRevision;
                d.cachedSortCatalogRevision = catalogRevision;

                // 2. Run Filter and rebuild d.filteredIndices
                d.filteredIndices.clear();
                auto checkMatch = [&](size_t idx) {
                    if (idx >= tickets.size())
                        return false;
                    if (d.gridFilterBuf[0] == '\0')
                        return true;
                    const auto& t = tickets[idx];
                    if (ContainsCaseInsensitive(t.id, d.gridFilterBuf))
                        return true;
                    if (ContainsCaseInsensitive(t.GetFieldValue("summary"), d.gridFilterBuf))
                        return true;
                    return false;
                };

                for (size_t idx : d.cachedSortedIndices) {
                    if (checkMatch(idx)) {
                        d.filteredIndices.push_back(idx);
                    }
                }

                // snprintf guarantees null-termination and avoids the strncpy
                // truncation warning when the source fills the buffer exactly.
                std::snprintf(lastFilter, sizeof(lastFilter), "%s", d.gridFilterBuf);
            }
        }

        // Rectangular selection invalidation: anchor/extent are expressed in
        // current sort-order row indices, so any change to sort order or ticket
        // set must clear the selection to avoid nonsense highlights / copies.
        gridSortSig = ComputeGridSortSignature(d.cachedSortFingerprint, app.GetActiveTicketsRevision(), tickets.size());
        if (d.gridState.RectSel.HasAnySelection() && d.gridState.RectSel.SortSignature != gridSortSig) {
            d.gridState.RectSel.ClearAll();
        }

        // Shared per-cell hit-test + overlay helper. Implements Google-Sheets-
        // style selection gestures:
        //   - Key (ID) column: plain click selects a single row; drag selects a
        //     contiguous range; Shift+click extends from the anchor; Ctrl+click
        //     toggles individual rows (non-contiguous selections).
        //   - Data cells: click activates the row and clears row selections.
        // Dragging continues to update while the mouse stays pressed even if
        // the user releases the modifier key mid-drag.
        auto handleCellRectSel = [&](int rowIdx, int colIdx, const ImVec2& cellOrigin, float cellWidth,
                                     const ImVec2& groupMin, const ImVec2& groupMax, bool isIdCol,
                                     const std::string& issueId, bool activeIssueWasThisRow) {
            const bool shiftHeld = ImGuiEffectiveKeyShift();
            const bool ctrlHeld = ImGuiEffectiveKeyCtrl();
            const ImVec2 hitMin(cellOrigin.x, groupMin.y);
            const ImVec2 hitMax(cellOrigin.x + cellWidth, groupMax.y);
            auto& sel = d.gridState.RectSel;
            const bool hovering = ImGui::IsMouseHoveringRect(hitMin, hitMax, false);
            const bool mouseClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

            if (hovering && mouseClick && sel.Contains(rowIdx, colIdx)) {
                rectCellClickedThisFrame = true;
            }

            if (hovering && mouseClick) {
                if (!issueId.empty()) {
                    d.gridState.SetActiveIssue(issueId);
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
                                d.gridState.ActiveIssueId.clear();
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
        };

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.rows");
            const size_t oldFilteredCount = d.filteredIndices.size();
            d.filteredIndices.erase(std::remove_if(d.filteredIndices.begin(), d.filteredIndices.end(),
                                                   [&](size_t idx) { return idx >= tickets.size(); }),
                                    d.filteredIndices.end());
            if (d.filteredIndices.size() != oldFilteredCount) {
                d.gridState.RectSel.ClearAll();
            }
            const std::vector<size_t>& indicesToUse = d.filteredIndices;

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
                    bool isActiveIssue = (d.gridState.ActiveIssueId == ticket.id);
                    const bool idKeySelectableSelected = d.gridState.RectSel.RowSelected(clippedRow);
                    const bool activeIssueWasThisRow = isActiveIssue;
                    if (isActiveIssue && !readOnlyMode) {
                        app.WarmIssueEditMetaAsync(ticket.id);
                    }
                    thread_local char rowPerfLabel[288];
                    const char* rowPerfScopeName = nullptr;
                    if (isActiveIssue) {
                        std::snprintf(rowPerfLabel, sizeof(rowPerfLabel), "activeProject:row[%zu] %s", r,
                                      ticket.id.c_str());
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
                        ImGui::TableSetBgColor(
                            ImGuiTableBgTarget_RowBg0,
                            ImGui::GetColorU32(ImVec4(statusColor.x, statusColor.y, statusColor.z, 0.12f)));
                    }

                    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
                        const auto& column = columns[static_cast<size_t>(colIndex)];
                        if (!ImGui::TableSetColumnIndex(colIndex)) {
                            if (!d.gridState.IsEditingField(ticket.id, column.FieldId)) {
                                // Horizontally clipped columns must still contribute layout height or row height
                                // tracks only visible cells (ImGui::TableSetColumnIndex doc).
                                ImGui::Dummy(ImVec2(1.0f, kTicketGridRowH));
                                continue;
                            }
                        }

                        // Captured before any widget advances the cursor so rect-
                        // select hit boxes cover the entire column cell area
                        // (not just the rendered text / widget extent).
                        const ImVec2 cellOriginForSel = ImGui::GetCursorScreenPos();
                        const float cellWidthForSel = ImGui::GetContentRegionAvail().x;
                        ImVec2 cellGroupMin(0.0f, 0.0f);
                        ImVec2 cellGroupMax(0.0f, 0.0f);

                        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                            ImGui::BeginGroup();
                            if (ImGui::Selectable(ticket.id.c_str(), idKeySelectableSelected,
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                                if (!ImGuiEffectiveKeyCtrl()) {
                                    d.gridState.SetActiveIssue(ticket.id);
                                }
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    const std::string url = app.BuildIssueBrowseUrl(d.cfg, ticket.id);
                                    app.OpenUrl(url);
                                }
                            }
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(BuildCellKey(ticket.id, "id"), ticket.id, std::string(),
                                                         column.Label, ticket.id, std::string(), &app, &d, readOnlyMode,
                                                         &ticket);
                            handleCellRectSel(clippedRow, colIndex, cellOriginForSel, cellWidthForSel, cellGroupMin,
                                              cellGroupMax, true, ticket.id, activeIssueWasThisRow);
                            continue;
                        }

                        const std::string currentValue = ticket.GetFieldValue(column.FieldId);
                        const TrackerField* fieldMeta = catalogIndex.Find(column.FieldId);
                        const float cellStartY = ImGui::GetCursorScreenPos().y;
                        const float cellRightX = ImGui::GetCursorScreenPos().x + cellWidthForSel;
                        const float valueAvailWidth = cellRightX - ImGui::GetCursorScreenPos().x;
                        const std::string cellKey = BuildCellKey(ticket.id, column.FieldId);
                        // Skip map lookup when feedback map is empty (common case) — avoids the hash (§3.1 item 57).
                        const auto feedbackIt =
                            d.cellFeedbackByKey.empty() ? d.cellFeedbackByKey.end() : d.cellFeedbackByKey.find(cellKey);
                        const bool isSavingThisCell = feedbackIt != d.cellFeedbackByKey.end() &&
                                                      feedbackIt->second.State == CellWriteState::Saving;

                        bool showBadge = false;
                        ImVec4 badgeColor(1.0f, 1.0f, 1.0f, 1.0f);
                        std::string badgeText;
                        std::string badgeTooltip;
                        if (feedbackIt != d.cellFeedbackByKey.end() &&
                            feedbackIt->second.State == CellWriteState::Error && !feedbackIt->second.Message.empty()) {
                            showBadge = true;
                            badgeColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                            badgeText = "!";
                            badgeTooltip = feedbackIt->second.Message;
                        } else if (feedbackIt != d.cellFeedbackByKey.end() &&
                                   feedbackIt->second.State == CellWriteState::Success) {
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
                                column.FieldId, fieldMeta, currentValue, d.cfg.DateFormatOption,
                                d.cfg.DateCompactRelativeThresholdDays);
                            const std::string* saveTip = column.IsDateLike ? &currentValue : nullptr;
                            const bool isDescriptionField =
                                !column.FieldId.empty() && (column.FieldId.find("description") != std::string::npos ||
                                                            column.FieldId.find("Description") != std::string::npos);
                            RenderClippedFieldText(saveDisplay, valueAvailWidth, d.cfg.EnableFieldOverflowTooltips,
                                                   true, saveTip, isDescriptionField, &column.FieldId);
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                                         ticket.GetFieldRichValue(column.FieldId), &app, &d,
                                                         readOnlyMode, &ticket);
                        } else {
                            const bool allowEditsForCell =
                                !readOnlyMode && column.NeedsAllowEditsCheck &&
                                app.CanEditFieldForIssue(ticket.id, column.FieldId, fieldMeta);
                            ImGui::BeginGroup();
                            TicketFieldEditor::RenderFieldCell(
                                app, ticket, column, colIndex, fieldMeta, currentValue, valueAvailWidth,
                                d.cfg.EnableFieldOverflowTooltips, allowEditsForCell, d.gridState, pendingEdits,
                                d.trackerGridAsync, d.cfg.DateFormatOption, d.cfg.DateCompactRelativeThresholdDays);
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                                         ticket.GetFieldRichValue(column.FieldId), &app, &d,
                                                         readOnlyMode, &ticket);
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

                        handleCellRectSel(clippedRow, colIndex, cellOriginForSel, cellWidthForSel, cellGroupMin,
                                          cellGroupMax, false, ticket.id, false);
                    }
                }
            }
        }

        if (!readOnlyMode) {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.newIssue");
            const CachedTicket* lastVisibleTicket = nullptr;
            if (!tickets.empty()) {
                size_t lastIndex = tickets.size() - 1;
                if (!d.filteredIndices.empty()) {
                    lastIndex = d.filteredIndices.back();
                }
                if (lastIndex < tickets.size()) {
                    lastVisibleTicket = &tickets[lastIndex];
                }
            }
            RenderNewIssueDraftRow(app, d, columns, d.cfg, lastVisibleTicket);
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.post");
            RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGui::GetCurrentTable(), d);

            // Capture column widths and sort specs into the active view IN MEMORY so the
            // grid renders the user's drag/sort immediately. The full unsaved-layout strip
            // (see drawActiveProjectWindow above) gates these changes behind Save / Discard;
            // the snapshot taken on first mutation lets Discard revert.
            if (activeViewForGrid) {
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
        // After full table layout: union outer + inner clip so empty scroll body
        // and padding still count as "inside grid" (OuterRect alone missed H5).
        if (ImGuiContext* g = ImGui::GetCurrentContext()) {
            if (ImGuiTable* tb = g->CurrentTable) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImRect hit(tb->OuterRect);
                    hit.Add(tb->InnerClipRect);
                    if (hit.Contains(ImGui::GetIO().MousePos)) {
                        ticketGridLeftClickInsideTableHit = true;
                        rectCellClickedThisFrame = true;
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // Google-Sheets-style selection: end drag on mouse release, clear when the
    // user clicks outside the current selection, and service Ctrl+C (copy as
    // TSV), Escape (clear), and Shift+Space (select whole row of active cell)
    // while the Active Project window is focused. Only runs when the grid was
    // drawn this frame (otherwise there is no valid sort signature).
    if (!columns.empty()) {
        SMATCHET_UI_PERF_SCOPE("activeProject:grid.rectSel.keys");
        auto& sel = d.gridState.RectSel;
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
        if ((sel.HasAnySelection() || !d.gridState.ActiveIssueId.empty()) && !effShift && !effCtrl && windowHovered &&
            !io.WantTextInput && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !rectCellClickedThisFrame &&
            !ticketGridLeftClickInsideTableHit) {
            sel.ClearAll();
            d.gridState.ActiveIssueId.clear();
        }
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (windowFocused && sel.HasAnySelection()) {
            if (!io.WantTextInput && effCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                CopyGridRectAsTsv(tickets, d.filteredIndices, columns, catalogIndex, sel);
            }
            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                sel.ClearAll();
            }
        }
        // Shift+Space: promote the row containing the active cell to a whole-
        // row selection (in addition to whatever is already selected).
        if (windowFocused && !io.WantTextInput && effShift && !effCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            int activeRow = -1;
            if (sel.PrimaryRow >= 0) {
                activeRow = sel.PrimaryRow;
            } else if (sel.Active) {
                activeRow = sel.AnchorRow;
            } else if (!d.gridState.ActiveIssueId.empty()) {
                const auto& indices = d.filteredIndices;
                for (size_t i = 0; i < indices.size(); ++i) {
                    const size_t ti = indices[i];
                    if (ti < tickets.size() && tickets[ti].id == d.gridState.ActiveIssueId) {
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
    }

    // Long-text / ADF modal editor lives at top-level so it survives the originating cell scrolling out
    // of view. Edits accepted in the modal are appended to `pendingEdits` and flow through the same
    // ProcessGridFieldEdits path below.
    TicketFieldEditor::RenderLongTextModal(pendingEdits);

    ProcessGridFieldEdits(app, d, tickets, pendingEdits, readOnlyMode);
    MaybeToastGridBannerFromSession(d);
    ImGui::End();
}
