#pragma once

// Ticket-grid column interaction over ImGui table internals: header drag-reorder driver and the
// per-index width sync. Decisions live in GridColumnOrderPure.h. Private to Source/Core/src/Ui.

#include <vector>

struct ImGuiTable;

namespace smatchet {
namespace ui {

/// Screen-space horizontal range that scrolls: right of any frozen columns, to the inner edge.
void GridScrollableStrip(const ImGuiTable& table, float& minX, float& maxX);

/// Moves a held header to the slot under the mouse and auto-scrolls near/past the strip edges.
/// Visual only; call after the header row. The order is committed once the drag ends.
void DriveGridHeaderDragReorder(ImGuiTable* table);

/// True while a header is held or a move is queued; the display order is final when false.
bool GridHeaderDragInProgress(const ImGuiTable* table);

/// Pushes per-index widths onto the table (ImGui keeps widths by index and only applies
/// TableSetupColumn's width at init), skipping a column being resized or auto-fitted.
/// Call between TableSetupColumn and the first TableNextRow.
void SyncGridTableColumnWidths(ImGuiTable* table, const std::vector<float>& widthsByIndex);

} // namespace ui
} // namespace smatchet
