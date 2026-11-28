#ifndef SMATCHET_UI_GRID_CURSOR_PURE_H
#define SMATCHET_UI_GRID_CURSOR_PURE_H

// Pure cell-cursor math for the ticket grid (docs/plans/shipped/input-ux-w3-grid-spreadsheet.md).
// Header-only, no ImGui, C++14. The grid draws the active cell from these positions; the keys that move it
// (arrows, Home / End, Ctrl+Home / End, PageUp / PageDown, Tab) call Step; a commit-and-move that must skip
// read-only cells calls StepToEditable; Shift+arrow grows a range with Extend; Ctrl+V lays the clipboard's
// text onto the grid with PlanPaste. Rows and columns are the shown (sorted, filtered) row index and the
// drawn column index, both 0-based.

#include "StringUtil.h" // UnguardSpreadsheetFormula

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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

// A range's membership as GridRectSelection::Remap builds it: one byte per shown row / column, nonzero = member.
// Passed instead of two corners, so a range that a re-sort split (rows A, B shown as A, C, B) never fills C.
struct RangeMask {
    const std::vector<char>* RowIn = nullptr;
    const std::vector<char>* ColIn = nullptr;
};

struct PasteCell {
    int Row = -1;
    int Col = -1;
    std::string Text;
};

// The most cells one paste plans. Every planned cell is read and pre-flighted on the UI thread before the confirm
// shows, so a whole-sheet block is cut here and the rest counted as dropped.
constexpr std::size_t kMaxPasteCells = 2000;

struct PastePlan {
    std::vector<PasteCell> Cells;
    int Dropped = 0; ///< pasted values that fell outside the grid or past kMaxPasteCells
};

namespace detail {

// The closing quote of the quoted cell that opens at `open`, or npos. A doubled quote is one literal quote.
// Over a whole text these scans stay linear: a later cell that opens inside an earlier scan sits on a quote
// run that scan read in pairs, so its own scan ends inside that run.
inline std::size_t ClosingQuote(const std::string& tsv, std::size_t open, bool& holdsBreak) {
    holdsBreak = false;
    for (std::size_t i = open + 1; i < tsv.size(); ++i) {
        const char ch = tsv[i];
        if (ch == '\t' || ch == '\n') {
            holdsBreak = true;
        } else if (ch == '"') {
            if (i + 1 < tsv.size() && tsv[i + 1] == '"') {
                ++i;
            } else {
                return i;
            }
        }
    }
    return std::string::npos;
}

// True when a cell can end at `pos`: the end of the text, a tab, or a line break (LF or CRLF).
inline bool EndsPasteCell(const std::string& tsv, std::size_t pos) {
    if (pos >= tsv.size() || tsv[pos] == '\t' || tsv[pos] == '\n') {
        return true;
    }
    return tsv[pos] == '\r' && (pos + 1 >= tsv.size() || tsv[pos + 1] == '\n');
}

// A quoted body as its value: a doubled quote reads as one quote, CRLF as LF.
inline std::string UnquotePasteBody(const std::string& tsv, std::size_t from, std::size_t to) {
    std::string out;
    out.reserve(to - from);
    for (std::size_t i = from; i < to; ++i) {
        if (tsv[i] == '\r' && i + 1 < to && tsv[i + 1] == '\n') {
            continue;
        }
        out.push_back(tsv[i]);
        if (tsv[i] == '"') {
            ++i; // the second quote of the pair
        }
    }
    return out;
}

// Reads the cell at `pos` and moves `pos` past its separator, which is returned in `sep` (tab, LF, or 0 at the
// end). A cell is unquoted only when it holds a tab or a line break, as a spreadsheet writes it; this app's own
// copy never writes those into a cell, so a copied value that starts and ends with a quote keeps both.
inline std::string NextPasteCell(const std::string& tsv, std::size_t& pos, char& sep) {
    std::string text;
    bool holdsBreak = false;
    const bool opens = pos < tsv.size() && tsv[pos] == '"';
    const std::size_t close = opens ? ClosingQuote(tsv, pos, holdsBreak) : std::string::npos;
    if (close != std::string::npos && holdsBreak && EndsPasteCell(tsv, close + 1)) {
        text = UnquotePasteBody(tsv, pos + 1, close);
        pos = close + 1;
        if (pos < tsv.size() && tsv[pos] == '\r') {
            ++pos;
        }
    } else {
        std::size_t end = tsv.find_first_of("\t\n", pos);
        end = end == std::string::npos ? tsv.size() : end;
        const bool carriage = end > pos && tsv[end - 1] == '\r' && (end == tsv.size() || tsv[end] == '\n');
        text = tsv.substr(pos, end - pos - (carriage ? 1 : 0));
        pos = end;
    }
    sep = pos < tsv.size() ? tsv[pos++] : '\0';
    return UnguardSpreadsheetFormula(text);
}

inline void FillRangeMembers(const std::string& value, const RangeMask& fill, int rowCount, int colCount,
                             PastePlan& plan) {
    if (fill.RowIn == nullptr || fill.ColIn == nullptr || rowCount < 0 || colCount < 0 ||
        fill.RowIn->size() != static_cast<std::size_t>(rowCount) ||
        fill.ColIn->size() != static_cast<std::size_t>(colCount)) {
        return;
    }
    for (int r = 0; r < rowCount; ++r) {
        if (!(*fill.RowIn)[static_cast<std::size_t>(r)]) {
            continue;
        }
        for (int c = 0; c < colCount; ++c) {
            if (!(*fill.ColIn)[static_cast<std::size_t>(c)]) {
                continue;
            }
            if (plan.Cells.size() >= kMaxPasteCells) {
                ++plan.Dropped;
                continue;
            }
            plan.Cells.push_back(PasteCell{r, c, value});
        }
    }
}

} // namespace detail

// A TSV block (the format CopyGridRectAsTsv writes) laid onto the grid at `at`. One value with a `fill` mask goes
// to every member cell (RowIn[r] && ColIn[c]) and nowhere else; one value without a mask goes to `at`. A block is
// placed from `at` over the shown cells and clipped to the grid; the mask does not shape it. Values outside the
// grid, and values past kMaxPasteCells planned cells, are dropped and counted; a mask whose sizes differ from
// rowCount / colCount fills nothing. The formula guard the copy adds is removed. A final line break ends the last
// row, it does not start another.
inline PastePlan PlanPaste(const std::string& tsv, CellPos at, const RangeMask* fill, int rowCount, int colCount) {
    PastePlan plan;
    if (tsv.empty()) {
        return plan;
    }
    std::size_t pos = 0;
    char sep = '\0';
    std::string text = detail::NextPasteCell(tsv, pos, sep);
    if (fill != nullptr && sep != '\t' && pos >= tsv.size()) {
        detail::FillRangeMembers(text, *fill, rowCount, colCount, plan);
        return plan;
    }
    const bool inside = at.Valid() && at.Row < rowCount && at.Col < colCount;
    int row = 0;
    int col = 0;
    for (;;) {
        if (inside && row < rowCount - at.Row && col < colCount - at.Col && plan.Cells.size() < kMaxPasteCells) {
            PasteCell cell;
            cell.Row = at.Row + row;
            cell.Col = at.Col + col;
            cell.Text = std::move(text);
            plan.Cells.push_back(std::move(cell));
        } else {
            ++plan.Dropped;
        }
        if (sep == '\t') {
            ++col;
        } else if (sep == '\n' && pos < tsv.size()) {
            ++row;
            col = 0;
        } else {
            return plan;
        }
        text = detail::NextPasteCell(tsv, pos, sep);
    }
}

} // namespace cursor
} // namespace grid
} // namespace smatchet

#endif // SMATCHET_UI_GRID_CURSOR_PURE_H
