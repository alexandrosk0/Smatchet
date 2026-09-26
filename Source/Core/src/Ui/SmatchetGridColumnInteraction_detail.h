#pragma once

// Ticket-grid column interaction on top of ImGui's table internals: the header drag-reorder
// driver and the per-index width sync. Decisions live in GridColumnOrderPure.h; this layer only
// reads and writes ImGuiTable state. Private to Source/Core/src/Ui.

#include <vector>

struct ImGuiTable;

namespace smatchet {
namespace ui {

/// Moves a held header to the slot under the mouse and auto-scrolls the table while the header
/// is held near or past either edge of its scrollable strip. Visual only — call after the header
/// row and before EndTable; the resulting order is committed once the drag ends
/// (GridHeaderDragInProgress turns false).
void DriveGridHeaderDragReorder(ImGuiTable* table);

/// True while a header is held or a header move is queued for the next frame. The table's
/// display order is only final — and safe to persist — when this is false.
bool GridHeaderDragInProgress(const ImGuiTable* table);

/// Pushes @p widthsByIndex (the width each column should render at, by column index) onto the
/// table. ImGui keeps widths per column index and only takes TableSetupColumn's width while the
/// table initialises, so without this a reorder, an added/removed column, or a view switch that
/// reuses the table leaves each index with the previous occupant's width. Skips the column being
/// resized or auto-fitted right now — that width flows the other way (grid post capture).
/// Call between the TableSetupColumn calls and the first TableNextRow.
void SyncGridTableColumnWidths(ImGuiTable* table, const std::vector<float>& widthsByIndex);

} // namespace ui
} // namespace smatchet
