#ifndef SMATCHET_UI_GRID_CURSOR_PURE_H
#define SMATCHET_UI_GRID_CURSOR_PURE_H

// Pure cell-cursor math for the ticket grid (docs/plans/active/input-ux-w3-grid-spreadsheet.md, slice S1).
// Header-only, no ImGui, C++14. The grid draws the active cell from these positions; the keys that move it
// (arrows, Home / End, Ctrl+Home / End, PageUp / PageDown, Tab) call Step; a commit-and-move that must skip
// read-only cells calls StepToEditable; Shift+arrow grows a range with Extend. Rows and columns are the
// shown (sorted, filtered) row index and the drawn column index, both 0-based.

namespace smatchet {
namespace grid {
namespace cursor {

struct CellPos {
    int Row = -1;
    int Col = -1;
    bool Valid() const { return Row >= 0 && Col >= 0; }
    bool operator==(const CellPos& o) const { return Row == o.Row && Col == o.Col; }
    bool operator!=(const CellPos& o) const { return !(*this == o); }
};

enum class Move {
    Up,
    Down,
    Left,
    Right,
    Home,   // first column of the row
    End,    // last column of the row
    Top,    // Ctrl+Home: the first cell of the grid
    Bottom, // Ctrl+End: the last cell of the grid
    PageUp,
    PageDown,
    TabNext, // next column; wraps to the first column of the next row
    TabPrev  // previous column; wraps to the last column of the previous row
};

namespace detail {
inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace detail

// One step inside [0,rowCount) x [0,colCount). A move that would leave the grid stays on the edge, except
// Tab, which wraps to the next / previous row (and stays put at the very first / last cell). An empty grid
// returns an invalid position. A `from` outside the grid (no active cell yet, or a stale one) lands on the
// first cell for every move, so the first key press selects a cell instead of moving from nowhere.
inline CellPos Step(CellPos from, Move m, int rowCount, int colCount, int pageRows) {
    if (rowCount <= 0 || colCount <= 0) {
        return CellPos();
    }
    if (!from.Valid() || from.Row >= rowCount || from.Col >= colCount) {
        return CellPos{0, 0};
    }
    const int lastRow = rowCount - 1;
    const int lastCol = colCount - 1;
    const int page = pageRows < 1 ? 1 : pageRows;
    CellPos to = from;
    switch (m) {
    case Move::Up:
        to.Row = detail::Clamp(from.Row - 1, 0, lastRow);
        break;
    case Move::Down:
        to.Row = detail::Clamp(from.Row + 1, 0, lastRow);
        break;
    case Move::Left:
        to.Col = detail::Clamp(from.Col - 1, 0, lastCol);
        break;
    case Move::Right:
        to.Col = detail::Clamp(from.Col + 1, 0, lastCol);
        break;
    case Move::Home:
        to.Col = 0;
        break;
    case Move::End:
        to.Col = lastCol;
        break;
    case Move::Top:
        to = CellPos{0, 0};
        break;
    case Move::Bottom:
        to = CellPos{lastRow, lastCol};
        break;
    case Move::PageUp:
        to.Row = detail::Clamp(from.Row - page, 0, lastRow);
        break;
    case Move::PageDown:
        to.Row = detail::Clamp(from.Row + page, 0, lastRow);
        break;
    case Move::TabNext:
        if (from.Col < lastCol) {
            to.Col = from.Col + 1;
        } else if (from.Row < lastRow) {
            to.Row = from.Row + 1;
            to.Col = 0;
        }
        break;
    case Move::TabPrev:
        if (from.Col > 0) {
            to.Col = from.Col - 1;
        } else if (from.Row > 0) {
            to.Row = from.Row - 1;
            to.Col = lastCol;
        }
        break;
    }
    return to;
}

// Commit-and-move target: steps like Step, then keeps stepping while `editable(row, col)` rejects the cell,
// for at most one full sweep of the grid. Returns `from` when no editable cell lies that way (the edge was
// reached first), so a commit never moves onto a read-only cell and never loops.
template <class Editable> CellPos StepToEditable(CellPos from, Move m, int rowCount, int colCount, Editable editable) {
    if (rowCount <= 0 || colCount <= 0 || !from.Valid()) {
        return from;
    }
    CellPos pos = from;
    const long long budget = static_cast<long long>(rowCount) * static_cast<long long>(colCount);
    for (long long i = 0; i < budget; ++i) {
        const CellPos next = Step(pos, m, rowCount, colCount, 1);
        if (next == pos) {
            return from; // the edge: nothing editable that way
        }
        pos = next;
        if (editable(pos.Row, pos.Col)) {
            return pos;
        }
    }
    return from;
}

struct Range {
    CellPos Anchor;
    CellPos Extent;
    int MinRow() const { return Anchor.Row < Extent.Row ? Anchor.Row : Extent.Row; }
    int MaxRow() const { return Anchor.Row > Extent.Row ? Anchor.Row : Extent.Row; }
    int MinCol() const { return Anchor.Col < Extent.Col ? Anchor.Col : Extent.Col; }
    int MaxCol() const { return Anchor.Col > Extent.Col ? Anchor.Col : Extent.Col; }
};

// Shift+arrow: the extent moves like a cursor, the anchor stays. A range with no extent yet starts from its
// anchor (a one-cell range grows from the cell it sits on).
inline Range Extend(const Range& r, Move m, int rowCount, int colCount, int pageRows) {
    Range out = r;
    const CellPos from = r.Extent.Valid() ? r.Extent : r.Anchor;
    out.Extent = Step(from, m, rowCount, colCount, pageRows);
    if (!out.Anchor.Valid()) {
        out.Anchor = out.Extent;
    }
    return out;
}

} // namespace cursor
} // namespace grid
} // namespace smatchet

#endif // SMATCHET_UI_GRID_CURSOR_PURE_H
