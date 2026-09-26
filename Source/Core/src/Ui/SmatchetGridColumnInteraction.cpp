#include "SmatchetGridColumnInteraction_detail.h"

#include "GridColumnOrderPure.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <vector>

namespace smatchet {
namespace ui {

namespace {

// Edge band and base speed for header-drag auto-scroll, in font-size units so they scale with
// the UI font: ~40px band and ~480px/s at the edge with a 16px font.
constexpr float kAutoScrollEdgeZoneEm = 2.5f;
constexpr float kAutoScrollSpeedAtEdgeEmPerSec = 30.0f;

} // namespace

void GridScrollableStrip(const ImGuiTable& table, float& minX, float& maxX) {
    minX = table.InnerClipRect.Min.x;
    maxX = table.InnerClipRect.Max.x;
    for (int order = 0; order < table.FreezeColumnsRequest && order < table.ColumnsCount; ++order) {
        minX = ImMax(minX, table.Columns[table.DisplayOrderToIndex[order]].MaxX);
    }
}

// ImGui's own header drag moves the held column one neighbour per frame, only while the mouse
// is moving, and never scrolls the table (imgui_tables.cpp: "FIXME-TABLE: Scroll request while
// reordering a column and it lands out of the scrolling zone"), so a column could not travel
// past what fit on screen. Queued after ImGui's own per-header request, so this one wins.
void DriveGridHeaderDragReorder(ImGuiTable* table) {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    if (table == nullptr || g == nullptr || (table->Flags & ImGuiTableFlags_Reorderable) == 0 ||
        table->HeldHeaderColumn < 0 || g->DragDropActive || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        return;
    }
    const int count = table->ColumnsCount;
    // Hidden columns would need order<->enabled-set mapping; the grid never hides columns, so
    // leave that case to ImGui's native behaviour rather than guess.
    if (count <= 1 || table->ColumnsEnabledCount != count || count > table->DisplayOrderToIndex.size() ||
        count > table->Columns.size()) {
        return;
    }
    const int held = table->HeldHeaderColumn;
    if (held >= count) {
        return;
    }
    const int heldOrder = table->Columns[held].DisplayOrder;
    if (heldOrder < table->FreezeColumnsRequest) {
        return; // frozen columns stay in the frozen strip
    }

    float stripMinX = 0.0f;
    float stripMaxX = 0.0f;
    GridScrollableStrip(*table, stripMinX, stripMaxX);
    const float mouseX = g->IO.MousePos.x;

    ImGuiWindow* inner = table->InnerWindow;
    if (inner != nullptr && (table->Flags & ImGuiTableFlags_ScrollX) != 0 && inner->ScrollMax.x > 0.0f) {
        const float fontSize = ImGui::GetFontSize();
        const float speed =
            GridHeaderDragAutoScrollSpeed(mouseX, g->IO.MouseClickedPos[ImGuiMouseButton_Left].x, stripMinX, stripMaxX,
                                          fontSize * kAutoScrollEdgeZoneEm, fontSize * kAutoScrollSpeedAtEdgeEmPerSec);
        if (speed != 0.0f) {
            const float target = ImClamp(inner->Scroll.x + speed * g->IO.DeltaTime, 0.0f, inner->ScrollMax.x);
            if (target != inner->Scroll.x) {
                ImGui::SetScrollX(inner, target);
            }
        }
    }

    std::vector<float> centers(static_cast<size_t>(count));
    for (int order = 0; order < count; ++order) {
        const ImGuiTableColumn& column = table->Columns[table->DisplayOrderToIndex[order]];
        centers[static_cast<size_t>(order)] = 0.5f * (column.MinX + column.MaxX);
    }
    // Past the strip's edge the probe stays at the edge: the column then advances one slot at a
    // time as the auto-scroll carries neighbours past the mouse, instead of leaping to the far end.
    const float probeX = ImClamp(mouseX, stripMinX, stripMaxX);
    const int targetOrder = GridHeaderDragTargetOrder(centers, heldOrder, probeX);
    // Also called when targetOrder == heldOrder: that cancels ImGui's own edge-crossing request
    // for this frame, so the two rules never disagree about where the column goes.
    ImGui::TableQueueSetColumnDisplayOrder(table, held, targetOrder);
}

bool GridHeaderDragInProgress(const ImGuiTable* table) {
    return table != nullptr && (table->HeldHeaderColumn != -1 || table->ReorderColumnDstOrder != -1);
}

void SyncGridTableColumnWidths(ImGuiTable* table, const std::vector<float>& widthsByIndex) {
    if (table == nullptr) {
        return;
    }
    const int count = ImMin(table->ColumnsCount, static_cast<int>(widthsByIndex.size()));
    for (int i = 0; i < count; ++i) {
        ImGuiTableColumn& column = table->Columns[i];
        if (i == table->LastResizedColumn || column.AutoFitQueue != 0 ||
            (column.Flags & ImGuiTableColumnFlags_WidthFixed) == 0) {
            continue;
        }
        const float want = widthsByIndex[static_cast<size_t>(i)];
        if (want > 0.0f && std::fabs(column.WidthRequest - want) > 0.5f) {
            column.WidthRequest = want;
        }
    }
}

} // namespace ui
} // namespace smatchet
