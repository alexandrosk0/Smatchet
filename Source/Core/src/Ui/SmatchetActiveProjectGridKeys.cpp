// SmatchetActiveProjectGridKeys.cpp — the keyboard cell cursor of the Active Project grid (input UX Wave 3,
// docs/plans/active/input-ux-w3-grid-spreadsheet.md, slice S1). A separate TU so SmatchetActiveProjectGridCells.cpp
// (the per-cell draw and the mouse gesture) does not grow past its cap.
//
// The grid keeps an ACTIVE CELL, the pair (issue id, field id) in SpreadsheetState, and a cell range. The arrow keys,
// Home / End, Ctrl+Home / Ctrl+End and PageUp / PageDown move the active cell (Shift extends the range); the math is
// the pure smatchet::grid::cursor header, tested in bucket A. Keys that start or end an edit (F2, Enter, a typed
// character, Tab, Ctrl+V / D / Z) come with later slices.

#include "SmatchetUI.h"
#include "SmatchetActiveProjectGridIds.h"
#include "SmatchetActiveProjectGridUi_Internal.h"

#include "CachedTicketTypes.h"
#include "GridPane.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetInputModifierBridge.h"
#include "SpreadsheetState.h"
#include "TicketGridModel.h"
#include "Ui/GridCursorPure.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstddef>
#include <cstdint>

namespace {

namespace cursor = smatchet::grid::cursor;

// The owner id the grid claims its cursor keys under. ImGui's generic nav reads an arrow with ImGuiKeyOwner_NoOwner,
// so a key owned by anyone else is invisible to it: while the grid is the focused window with no text input, one
// arrow press moves exactly one cell and no second (nav) cursor moves with it.
ImGuiID CursorKeysOwner() {
    static const ImGuiID owner = ImHashStr("smatchet.grid.cursorkeys");
    return owner;
}

struct CursorKey {
    ImGuiKey Key;
    cursor::Move Plain;
};

const CursorKey kCursorKeys[] = {
    {ImGuiKey_UpArrow, cursor::Move::Up},     {ImGuiKey_DownArrow, cursor::Move::Down},
    {ImGuiKey_LeftArrow, cursor::Move::Left}, {ImGuiKey_RightArrow, cursor::Move::Right},
    {ImGuiKey_Home, cursor::Move::Home},      {ImGuiKey_End, cursor::Move::End},
    {ImGuiKey_PageUp, cursor::Move::PageUp},  {ImGuiKey_PageDown, cursor::Move::PageDown},
};

// The move a pressed key stands for under the held modifiers; false when the chord is not a cursor move (Ctrl+arrow,
// anything with Alt: those belong to other handlers).
bool ResolveMove(const CursorKey& k, bool ctrl, bool alt, cursor::Move* out) {
    if (alt) {
        return false;
    }
    if (ctrl) {
        if (k.Key == ImGuiKey_Home) {
            *out = cursor::Move::Top;
            return true;
        }
        if (k.Key == ImGuiKey_End) {
            *out = cursor::Move::Bottom;
            return true;
        }
        return false;
    }
    *out = k.Plain;
    return true;
}

} // namespace

void ScrollGridCellIntoView(float minY, float maxY, int colIdx) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiTable* table = ImGui::GetCurrentTable();
    if (window == nullptr || table == nullptr || colIdx < 0 || colIdx >= table->ColumnsCount) {
        return;
    }
    // The column's laid-out span is known even when the column is scrolled out of view and its cells are not drawn.
    // ImGui's scroll-to-rect already keeps the rect clear of the table's frozen header row and frozen key column (the
    // table reports them as the inner window's decoration), so no inset is added here.
    const ImRect rect(table->Columns[colIdx].MinX, minY, table->Columns[colIdx].MaxX, maxY);
    ImGui::ScrollToRect(window, rect, ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
}

void SmatchetUI::drawActiveProjectGridCursorKeys(ActiveProjectDrawCtx& ctx) {
    GridPane& pane = ctx.pane;
    if (ctx.columns.empty() || pane.filteredIndices.empty()) {
        return;
    }
    SpreadsheetState& gs = pane.gridState;
    GridRectSelection& sel = gs.RectSel;
    const std::uint64_t sig = ctx.gridSortSig;
    const GridShownRowIds shownIds{pane.filteredIndices, ctx.tickets};
    const GridDrawnColumnFieldIds columnIds{ctx.columns};
    const int rowCount = static_cast<int>(pane.filteredIndices.size());
    const int colCount = static_cast<int>(ctx.columns.size());

    // A gesture this frame may have changed `Rows` (the key column, Ctrl+A, a menu): record it by identity now, while
    // it still addresses the order it was made in, so a re-sort next frame can carry it over.
    if (sel.SortSignature == sig) {
        sel.SyncRows(rowCount, shownIds);
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool eligible = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
                          gs.EditingId.empty() &&
                          !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!eligible) {
        return;
    }

    // Claim every cursor key under the grid's owner id (it must be set before ImGui's nav reads the key on the NEXT
    // frame, so it is renewed each frame the grid is eligible; ImGui releases a key's owner once the key is up).
    const ImGuiID owner = CursorKeysOwner();
    for (const CursorKey& k : kCursorKeys) {
        ImGui::SetKeyOwner(k.Key, owner);
    }

    const bool shift = ImGuiEffectiveKeyShift();
    const bool ctrl = ImGuiEffectiveKeyCtrl();
    const bool alt = io.KeyAlt;
    for (const CursorKey& k : kCursorKeys) {
        if (!ImGui::IsKeyPressed(k.Key, ImGuiInputFlags_Repeat, owner)) {
            continue;
        }
        cursor::Move move = cursor::Move::Down;
        if (!ResolveMove(k, ctrl, alt, &move)) {
            continue;
        }
        const cursor::CellPos active{gs.ActiveRow, gs.ActiveCol};
        if (shift) {
            // Shift+move grows the range from its anchor (from the active cell when there is no range yet); the
            // active cell itself stays where it is.
            cursor::Range range;
            range.Anchor = sel.Active ? cursor::CellPos{sel.AnchorRow, sel.AnchorCol} : active;
            range.Extent = sel.Active ? cursor::CellPos{sel.ExtentRow, sel.ExtentCol} : active;
            const cursor::Range grown = cursor::Extend(range, move, rowCount, colCount, gs.PageRows);
            if (!grown.Anchor.Valid() || !grown.Extent.Valid()) {
                return;
            }
            if (!active.Valid()) {
                gs.SetActiveCell(shownIds(grown.Anchor.Row), columnIds(grown.Anchor.Col), grown.Anchor.Row,
                                 grown.Anchor.Col, sig);
            }
            sel.Rows.clear();
            sel.PrimaryRow = -1;
            sel.SetRange(grown.Anchor.Row, grown.Anchor.Col, grown.Extent.Row, grown.Extent.Col, rowCount, shownIds,
                         colCount, columnIds);
            gs.ScrollToRow = grown.Extent.Row;
            gs.ScrollToCol = grown.Extent.Col;
        } else {
            const cursor::CellPos to = cursor::Step(active, move, rowCount, colCount, gs.PageRows);
            if (!to.Valid()) {
                return;
            }
            // A move collapses the selection to the destination cell (a spreadsheet's behaviour): the one-cell range
            // is what Ctrl+C copies, and Esc then clears it while the active cell stays.
            sel.ClearAll();
            gs.SetActiveCell(shownIds(to.Row), columnIds(to.Col), to.Row, to.Col, sig);
            sel.SetRange(to.Row, to.Col, to.Row, to.Col, rowCount, shownIds, colCount, columnIds);
            gs.ScrollToRow = to.Row;
            gs.ScrollToCol = to.Col;
        }
        sel.SortSignature = sig;
        break; // one move per frame, so a held key repeats at ImGui's key-repeat rate
    }
}
