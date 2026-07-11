#include "SmatchetUI.h"
#include "SmatchetActiveProjectGridUi_Internal.h"
#include "SmatchetGridPaneWindows.h" // detail::PaneViewSelfRepairAllowed (HIGH-1) + ChoosePaneColumnsSource
#include "SmatchetGridUiSupport.h"
#include "SmatchetViewsDashboardUi_detail.h"

// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=this sibling TU is a byte-identical god-file-split carve-out of SmatchetActiveProjectGridUi.cpp and needs the full AppController to drive ctx.app.* on the grid table/cell paths — same shape as the AnnotateAnalysisUi_Modals/_Window split; owner=orchestrator; revisit=when the grid draw context is narrowed to an interface)
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

