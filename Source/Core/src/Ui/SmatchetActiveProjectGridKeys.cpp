// SmatchetActiveProjectGridKeys.cpp — the keyboard cell cursor of the Active Project grid (input UX Wave 3,
// docs/plans/active/input-ux-w3-grid-spreadsheet.md, slice S1). A separate TU so SmatchetActiveProjectGridCells.cpp
// (the per-cell draw and the mouse gesture) does not grow past its cap.
//
// The grid keeps an ACTIVE CELL, the pair (issue id, field id) in SpreadsheetState, and a cell range. The arrow keys,
// Home / End, Ctrl+Home / Ctrl+End and PageUp / PageDown move the active cell (Shift extends the range); the math is
// the pure smatchet::grid::cursor header, tested in bucket A. Slice S2 adds the keys that start and end an edit: F2,
// Enter and a typed character open the active cell's editor; an inline editor that ended on Enter / Tab leaves a
// GridEditExit that moves the active cell (read-only cells skipped). Ctrl+V / D / Z come with later slices.

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
#include <string>

namespace {

namespace cursor = smatchet::grid::cursor;

// The owner id the grid claims its cursor keys under. ImGui's generic nav reads an arrow with ImGuiKeyOwner_NoOwner,
// so a key owned by anyone else is invisible to it: while the grid is the focused window with no text input, one
// arrow press moves exactly one cell and no second (nav) cursor moves with it.
ImGuiID CursorKeysOwner() {
    static const ImGuiID owner = ImHashStr("smatchet.grid.cursorkeys");
    return owner;
}

// The keys that start an edit. Claimed under the same owner while the grid is eligible, so ImGui's nav does not read
// Enter (or Space) as "activate the focused item" under the cell that is about to open its own editor.
const ImGuiKey kEditKeys[] = {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_F2, ImGuiKey_Space};

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

namespace {

// Makes (row, col) the active cell and the one-cell range (what a plain move does), and scrolls it into view.
void GoToCell(SpreadsheetState& gs, const GridShownRowIds& shownIds, const GridDrawnColumnFieldIds& columnIds, int row,
              int col, int rowCount, int colCount, std::uint64_t sig) {
    GridRectSelection& sel = gs.RectSel;
    // A move collapses the selection to the destination cell (a spreadsheet's behaviour): the one-cell range is what
    // Ctrl+C copies, and Esc then clears it while the active cell stays.
    sel.ClearAll();
    gs.SetActiveCell(shownIds(row), columnIds(col), row, col, sig);
    sel.SetRange(row, col, row, col, rowCount, shownIds, colCount, columnIds);
    gs.ScrollToRow = row;
    gs.ScrollToCol = col;
    sel.SortSignature = sig;
}

// The cell move an edit exit stands for, indexed by GridEditExit (None, Down, Up, Right, Left).
cursor::Move MoveForExit(GridEditExit exit) {
    static const cursor::Move kMoves[] = {cursor::Move::Down, cursor::Move::Down, cursor::Move::Up,
                                          cursor::Move::TabNext, cursor::Move::TabPrev};
    const std::size_t index = static_cast<std::size_t>(exit);
    return index < sizeof(kMoves) / sizeof(kMoves[0]) ? kMoves[index] : cursor::Move::Down;
}

// An inline editor ended on Enter / Tab: the active cell moves to the next EDITABLE cell that way, and the editor of
// that cell stays closed (type-to-edit is the fast path). Nothing editable that way (the edge, or a column of
// read-only cells): the active cell stays. At most kEditableChecks permission checks per press, so a sweep over a grid
// of read-only cells stays cheap.
void ApplyEditExit(ActiveProjectDrawCtx& ctx, const GridShownRowIds& shownIds, const GridDrawnColumnFieldIds& columnIds,
                   int rowCount, int colCount, std::uint64_t sig) {
    SpreadsheetState& gs = ctx.pane.gridState;
    const GridEditExit exit = gs.PendingEditExit;
    gs.PendingEditExit = GridEditExit::None;
    const cursor::CellPos from{gs.ActiveRow, gs.ActiveCol};
    if (!from.Valid()) {
        return;
    }
    constexpr int kEditableChecks = 4096;
    int checks = 0;
    const cursor::CellPos to =
        cursor::StepToEditable(from, MoveForExit(exit), rowCount, colCount, [&](int row, int col) {
            return ++checks <= kEditableChecks && GridCellEditable(ctx, row, col);
        });
    if (to != from) {
        GoToCell(gs, shownIds, columnIds, to.Row, to.Col, rowCount, colCount, sig);
    }
}

// The text a key press types into a cell: the printable characters of this frame's input queue. Nothing while Ctrl or
// Alt is held alone (a shortcut, not text; both together is AltGr, which types), and no Space with Shift (that is the
// promote-row-to-selection chord).
std::string TypedSeed(const ImGuiIO& io, bool shift) {
    if (io.KeyCtrl != io.KeyAlt || io.InputQueueCharacters.Size == 0) {
        return std::string();
    }
    ImVector<ImWchar> kept;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c < 0x20 || c == 0x7F || (c == ' ' && shift)) {
            continue;
        }
        kept.push_back(c);
    }
    if (kept.Size == 0) {
        return std::string();
    }
    char buf[512];
    ImTextStrToUtf8(buf, static_cast<int>(sizeof(buf)), kept.Data, kept.Data + kept.Size);
    return std::string(buf);
}

// F2 / Enter / a typed character on an editable active cell: leave a request for that cell's draw (the editor kinds
// each open through their own code). Returns true when a request was made (the key is spent).
bool RequestEditFromKeys(ActiveProjectDrawCtx& ctx, const GridDrawnColumnFieldIds& columnIds, const ImGuiIO& io,
                         bool shift, bool ctrl, bool alt) {
    SpreadsheetState& gs = ctx.pane.gridState;
    if (gs.ActiveIssueId.empty() || !GridCellEditable(ctx, gs.ActiveRow, gs.ActiveCol)) {
        return false;
    }
    GridEditStart kind = GridEditStart::None;
    std::string seed;
    if (!ctrl && !alt && !shift && ImGui::IsKeyPressed(ImGuiKey_F2, ImGuiInputFlags_None, CursorKeysOwner())) {
        kind = GridEditStart::CaretAtEnd;
    } else if (!ctrl && !alt && !shift &&
               (ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, CursorKeysOwner()) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, CursorKeysOwner()))) {
        kind = GridEditStart::SelectAll;
    } else {
        seed = TypedSeed(io, shift);
        if (!seed.empty()) {
            kind = GridEditStart::Typed;
        }
    }
    if (kind == GridEditStart::None) {
        return false;
    }
    gs.KeyEditKind = kind;
    gs.KeyEditIssueId = gs.ActiveIssueId;
    gs.KeyEditFieldId = columnIds(gs.ActiveCol);
    gs.KeyEditSeed = seed;
    gs.KeyEditFrames = 0;
    gs.ScrollToRow = gs.ActiveRow; // an off-screen active cell is brought into view so its draw can take the request
    gs.ScrollToCol = gs.ActiveCol;
    return true;
}

// One arrow / Home / End / Page key press this frame, if any, applied to the active cell or (Shift) to the range.
void ApplyCursorMoveKeys(ActiveProjectDrawCtx& ctx, const GridShownRowIds& shownIds,
                         const GridDrawnColumnFieldIds& columnIds, int rowCount, int colCount, std::uint64_t sig,
                         bool shift, bool ctrl, bool alt) {
    SpreadsheetState& gs = ctx.pane.gridState;
    GridRectSelection& sel = gs.RectSel;
    const ImGuiID owner = CursorKeysOwner();
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
            sel.SortSignature = sig;
        } else {
            const cursor::CellPos to = cursor::Step(active, move, rowCount, colCount, gs.PageRows);
            if (!to.Valid()) {
                return;
            }
            GoToCell(gs, shownIds, columnIds, to.Row, to.Col, rowCount, colCount, sig);
        }
        break; // one move per frame, so a held key repeats at ImGui's key-repeat rate
    }
}

} // namespace

void SmatchetUI::drawActiveProjectGridCursorKeys(ActiveProjectDrawCtx& ctx) {
    GridPane& pane = ctx.pane;
    SpreadsheetState& gs = pane.gridState;
    // A key-edit request waits a few frames for its cell: the cell may first have to be scrolled into view (the active
    // cell can be off-screen after a re-sort or a mouse scroll), and then be laid out. One no cell took by then is
    // dropped (the cell is read-only or its column is hidden).
    if (gs.KeyEditKind != GridEditStart::None && ++gs.KeyEditFrames > 3) {
        gs.ClearKeyEdit();
    }
    if (ctx.columns.empty() || pane.filteredIndices.empty()) {
        gs.PendingEditExit = GridEditExit::None;
        return;
    }
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
    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // An editor, an armed dropdown or a popup was open at the end of last frame: a key that closed it this frame
    // (Enter in a text cell, Enter / Esc in a dropdown) must not also start the next edit.
    // An arm whose popup is gone (it closed while its cell was clipped, filtered out or scrolled away, so the cell's
    // own draw never released it) would otherwise keep every key dead until that row is drawn again.
    if (!gs.EditArmedKey.empty() && !gs.EditArmedJustOpened && !popupOpen) {
        gs.EditArmedKey.clear();
    }
    const bool busyLastFrame = gs.EditBusyLastFrame;
    gs.EditBusyLastFrame = !gs.EditingId.empty() || !gs.EditArmedKey.empty() || popupOpen;

    if (gs.PendingEditExit != GridEditExit::None) {
        ApplyEditExit(ctx, shownIds, columnIds, rowCount, colCount, sig);
        return; // the Enter / Tab that ended the edit is spent
    }

    const bool eligible = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
                          gs.EditingId.empty() && gs.EditArmedKey.empty() && !popupOpen;
    if (!eligible) {
        return;
    }

    // Claim every cursor and edit key under the grid's owner id (it must be set before ImGui's nav reads the key on the
    // NEXT frame, so it is renewed each frame the grid is eligible; ImGui releases a key's owner once the key is up).
    const ImGuiID owner = CursorKeysOwner();
    for (const CursorKey& k : kCursorKeys) {
        ImGui::SetKeyOwner(k.Key, owner);
    }
    for (const ImGuiKey key : kEditKeys) {
        ImGui::SetKeyOwner(key, owner);
    }

    const bool shift = ImGuiEffectiveKeyShift();
    const bool ctrl = ImGuiEffectiveKeyCtrl();
    const bool alt = io.KeyAlt;
    if (!busyLastFrame && RequestEditFromKeys(ctx, columnIds, io, shift, ctrl, alt)) {
        return;
    }
    ApplyCursorMoveKeys(ctx, shownIds, columnIds, rowCount, colCount, sig, shift, ctrl, alt);
}
