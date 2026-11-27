// Bucket-A doctest for the grid's cell-cursor math (Ui/GridCursorPure.h): every move at the edges, Tab wrap, the
// skip-read-only sweep of a commit-and-move, range extend, and where a pasted text lands (PlanPaste). Header-only,
// no ImGui.

#include "Ui/GridCursorPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::grid::cursor::CellPos;
using smatchet::grid::cursor::Extend;
using smatchet::grid::cursor::Move;
using smatchet::grid::cursor::PasteCell;
using smatchet::grid::cursor::PastePlan;
using smatchet::grid::cursor::PlanPaste;
using smatchet::grid::cursor::Range;
using smatchet::grid::cursor::RangeMask;
using smatchet::grid::cursor::Step;
using smatchet::grid::cursor::StepToEditable;

namespace {
constexpr int kRows = 5;
constexpr int kCols = 4;
CellPos S(CellPos from, Move m, int page = 3) { return Step(from, m, kRows, kCols, page); }

// "r,c=text" of every planned cell, in plan order: one string a case can compare whole.
std::string Cells(const PastePlan& plan) {
    std::string out;
    for (const PasteCell& cell : plan.Cells) {
        out += std::to_string(cell.Row) + "," + std::to_string(cell.Col) + "=" + cell.Text + ";";
    }
    return out;
}
} // namespace

TEST_CASE("Step: arrows move one cell and clamp at every edge") {
    CHECK(S({2, 1}, Move::Up) == CellPos{1, 1});
    CHECK(S({2, 1}, Move::Down) == CellPos{3, 1});
    CHECK(S({2, 1}, Move::Left) == CellPos{2, 0});
    CHECK(S({2, 1}, Move::Right) == CellPos{2, 2});
    CHECK(S({0, 1}, Move::Up) == CellPos{0, 1});
    CHECK(S({kRows - 1, 1}, Move::Down) == CellPos{kRows - 1, 1});
    CHECK(S({2, 0}, Move::Left) == CellPos{2, 0});
    CHECK(S({2, kCols - 1}, Move::Right) == CellPos{2, kCols - 1});
}

TEST_CASE("Step: Home / End stay on the row, Top / Bottom are the first and last cell") {
    CHECK(S({3, 2}, Move::Home) == CellPos{3, 0});
    CHECK(S({3, 1}, Move::End) == CellPos{3, kCols - 1});
    CHECK(S({3, 2}, Move::Top) == CellPos{0, 0});
    CHECK(S({1, 1}, Move::Bottom) == CellPos{kRows - 1, kCols - 1});
}

TEST_CASE("Step: PageUp / PageDown move a page of rows and clamp; a page below one is one row") {
    CHECK(S({1, 2}, Move::PageDown, 3) == CellPos{4, 2});
    CHECK(S({4, 2}, Move::PageDown, 3) == CellPos{4, 2});
    CHECK(S({3, 2}, Move::PageUp, 2) == CellPos{1, 2});
    CHECK(S({1, 2}, Move::PageUp, 9) == CellPos{0, 2});
    CHECK(S({2, 2}, Move::PageDown, 0) == CellPos{3, 2});
    CHECK(S({2, 2}, Move::PageUp, -4) == CellPos{1, 2});
}

TEST_CASE("Step: Tab wraps to the next / previous row and stops at the very first / last cell") {
    CHECK(S({1, 1}, Move::TabNext) == CellPos{1, 2});
    CHECK(S({1, kCols - 1}, Move::TabNext) == CellPos{2, 0});
    CHECK(S({kRows - 1, kCols - 1}, Move::TabNext) == CellPos{kRows - 1, kCols - 1});
    CHECK(S({1, 1}, Move::TabPrev) == CellPos{1, 0});
    CHECK(S({1, 0}, Move::TabPrev) == CellPos{0, kCols - 1});
    CHECK(S({0, 0}, Move::TabPrev) == CellPos{0, 0});
}

TEST_CASE("Step: no cell yet, or a stale one, lands on the first cell; an empty grid has none") {
    CHECK(S(CellPos(), Move::Down) == CellPos{0, 0});
    CHECK(S({kRows, 0}, Move::Up) == CellPos{0, 0});
    CHECK(S({0, kCols}, Move::Left) == CellPos{0, 0});
    CHECK_FALSE(Step({0, 0}, Move::Down, 0, kCols, 3).Valid());
    CHECK_FALSE(Step({0, 0}, Move::Down, kRows, 0, 3).Valid());
}

TEST_CASE("StepToEditable: skips read-only cells in the direction of the move") {
    // Column 1 and 2 of row 2 are read-only; moving right from (2,0) lands on column 3.
    auto editable = [](int row, int col) { return !(row == 2 && (col == 1 || col == 2)); };
    CHECK(StepToEditable({2, 0}, Move::Right, kRows, kCols, editable) == CellPos{2, 3});
    // Down from (1,1) skips nothing in column 1 except row 2.
    CHECK(StepToEditable({1, 1}, Move::Down, kRows, kCols, editable) == CellPos{3, 1});
    CHECK(StepToEditable({3, 1}, Move::Up, kRows, kCols, editable) == CellPos{1, 1});
}

TEST_CASE("StepToEditable: Tab wraps past read-only cells onto the next row") {
    auto editable = [](int row, int col) { return !(row == 1 && col >= 2); };
    CHECK(StepToEditable({1, 1}, Move::TabNext, kRows, kCols, editable) == CellPos{2, 0});
}

TEST_CASE("StepToEditable: no editable cell that way returns where it started and does not loop") {
    auto none = [](int, int) { return false; };
    CHECK(StepToEditable({2, 1}, Move::Down, kRows, kCols, none) == CellPos{2, 1});
    CHECK(StepToEditable({2, 1}, Move::TabNext, kRows, kCols, none) == CellPos{2, 1});
    // The edge is reached before any editable cell.
    auto onlyTop = [](int row, int) { return row == 0; };
    CHECK(StepToEditable({3, 1}, Move::Down, kRows, kCols, onlyTop) == CellPos{3, 1});
    // An invalid start has nowhere to move from.
    CHECK_FALSE(StepToEditable(CellPos(), Move::Down, kRows, kCols, none).Valid());
}

TEST_CASE("Range: corners are order-independent") {
    Range r;
    r.Anchor = {3, 2};
    r.Extent = {1, 0};
    CHECK(r.MinRow() == 1);
    CHECK(r.MaxRow() == 3);
    CHECK(r.MinCol() == 0);
    CHECK(r.MaxCol() == 2);
}

TEST_CASE("Extend: the extent moves and the anchor stays") {
    Range r;
    r.Anchor = {1, 1};
    r.Extent = {1, 1};
    r = Extend(r, Move::Down, kRows, kCols, 3);
    r = Extend(r, Move::Right, kRows, kCols, 3);
    CHECK(r.Anchor == CellPos{1, 1});
    CHECK(r.Extent == CellPos{2, 2});
    r = Extend(r, Move::Up, kRows, kCols, 3);
    r = Extend(r, Move::Up, kRows, kCols, 3);
    CHECK(r.Anchor == CellPos{1, 1});
    CHECK(r.Extent == CellPos{0, 2});
    CHECK(r.MinRow() == 0);
    CHECK(r.MaxRow() == 1);
}

TEST_CASE("Extend: clamps at the grid edge, and a range without an extent grows from its anchor") {
    Range r;
    r.Anchor = {kRows - 1, kCols - 1};
    r = Extend(r, Move::Down, kRows, kCols, 3);
    CHECK(r.Extent == CellPos{kRows - 1, kCols - 1});
    Range fromAnchor;
    fromAnchor.Anchor = {2, 2};
    fromAnchor = Extend(fromAnchor, Move::Left, kRows, kCols, 3);
    CHECK(fromAnchor.Anchor == CellPos{2, 2});
    CHECK(fromAnchor.Extent == CellPos{2, 1});
}

TEST_CASE("PlanPaste: one value with no range goes to the cell it is pasted on") {
    const PastePlan plan = PlanPaste("x", {1, 2}, nullptr, kRows, kCols);
    CHECK(Cells(plan) == "1,2=x;");
    CHECK(plan.Dropped == 0);
    // The line break a grid copy ends with does not start a second row.
    CHECK(Cells(PlanPaste("x\n", {1, 2}, nullptr, kRows, kCols)) == "1,2=x;");
    CHECK(PlanPaste("", {1, 2}, nullptr, kRows, kCols).Cells.empty());
}

TEST_CASE("PlanPaste: one value over a range fills every member cell and no other") {
    const std::vector<char> rowIn = {0, 1, 1, 0, 0};
    const std::vector<char> colIn = {0, 1, 1, 0};
    RangeMask mask;
    mask.RowIn = &rowIn;
    mask.ColIn = &colIn;
    const PastePlan plan = PlanPaste("x\n", {4, 3}, &mask, kRows, kCols);
    CHECK(Cells(plan) == "1,1=x;1,2=x;2,1=x;2,2=x;");
    CHECK(plan.Dropped == 0);
}

TEST_CASE("PlanPaste: a range a re-sort split fills its members and never the row between them") {
    // Members A and B are shown as A, C, B: rows 0 and 2 are in, row 1 is not.
    const std::vector<char> rowIn = {1, 0, 1, 0, 0};
    const std::vector<char> colIn = {0, 1, 0, 0};
    RangeMask mask;
    mask.RowIn = &rowIn;
    mask.ColIn = &colIn;
    CHECK(Cells(PlanPaste("x", {0, 1}, &mask, kRows, kCols)) == "0,1=x;2,1=x;");
}

TEST_CASE("PlanPaste: a mask of the wrong size fills nothing") {
    const std::vector<char> shortRows = {1, 1, 1};
    const std::vector<char> colIn = {1, 1, 1, 1};
    RangeMask mask;
    mask.RowIn = &shortRows;
    mask.ColIn = &colIn;
    CHECK(PlanPaste("x", {0, 0}, &mask, kRows, kCols).Cells.empty());
    RangeMask unset;
    CHECK(PlanPaste("x", {0, 0}, &unset, kRows, kCols).Cells.empty());
}

TEST_CASE("PlanPaste: a block is placed from the cell and clipped to the grid, the excess counted") {
    const PastePlan plan = PlanPaste("a\tb\tc\nd\te\tf\n", {4, 2}, nullptr, kRows, kCols);
    CHECK(Cells(plan) == "4,2=a;4,3=b;");
    CHECK(plan.Dropped == 4);
    // The range does not shape a block: it lands from the cell, over cells outside the mask too.
    const std::vector<char> rowIn = {1, 0, 0, 0, 0};
    const std::vector<char> colIn = {1, 0, 0, 0};
    RangeMask mask;
    mask.RowIn = &rowIn;
    mask.ColIn = &colIn;
    CHECK(Cells(PlanPaste("a\tb\nc\td", {1, 1}, &mask, kRows, kCols)) == "1,1=a;1,2=b;2,1=c;2,2=d;");
    // No cell to paste onto: everything is dropped.
    const PastePlan nowhere = PlanPaste("a\tb", {-1, -1}, nullptr, kRows, kCols);
    CHECK(nowhere.Cells.empty());
    CHECK(nowhere.Dropped == 2);
}

TEST_CASE("PlanPaste: a paste plans at most kMaxPasteCells cells and counts the rest") {
    using smatchet::grid::cursor::kMaxPasteCells;
    const int cap = static_cast<int>(kMaxPasteCells);
    // A block: one column, cap + 5 rows, on a grid tall enough to hold them all.
    std::string column;
    for (int i = 0; i < cap + 5; ++i) {
        column += "v\n";
    }
    const PastePlan block = PlanPaste(column, {0, 0}, nullptr, cap + 100, 1);
    CHECK(block.Cells.size() == kMaxPasteCells);
    CHECK(block.Dropped == 5);
    CHECK(block.Cells.back().Row == cap - 1);
    // One value over a range of cap + 7 member cells.
    const std::vector<char> rowIn(static_cast<std::size_t>(cap + 7), 1);
    const std::vector<char> colIn = {1};
    RangeMask mask;
    mask.RowIn = &rowIn;
    mask.ColIn = &colIn;
    const PastePlan fill = PlanPaste("x", {0, 0}, &mask, cap + 7, 1);
    CHECK(fill.Cells.size() == kMaxPasteCells);
    CHECK(fill.Dropped == 7);
}

TEST_CASE("PlanPaste: ragged rows keep each value in its own column, an empty cell included") {
    CHECK(Cells(PlanPaste("a\tb\nc\n\td", {0, 0}, nullptr, kRows, kCols)) == "0,0=a;0,1=b;1,0=c;2,0=;2,1=d;");
}

TEST_CASE("PlanPaste: CRLF line ends read as LF") {
    CHECK(Cells(PlanPaste("a\tb\r\nc\td\r\n", {0, 0}, nullptr, kRows, kCols)) == "0,0=a;0,1=b;1,0=c;1,1=d;");
}

TEST_CASE("PlanPaste: a quoted cell is unquoted only when it holds a tab or a line break") {
    // A spreadsheet quotes a cell that holds a break, and doubles the quotes inside it.
    CHECK(Cells(PlanPaste("\"one\ntwo\"\tb", {0, 0}, nullptr, kRows, kCols)) == "0,0=one\ntwo;0,1=b;");
    CHECK(Cells(PlanPaste("\"say \"\"hi\"\"\tnow\"\tb", {0, 0}, nullptr, kRows, kCols)) ==
          "0,0=say \"hi\"\tnow;0,1=b;");
    // A value this app copied that merely starts and ends with a quote keeps both.
    CHECK(Cells(PlanPaste("\"abc\"\tb", {0, 0}, nullptr, kRows, kCols)) == "0,0=\"abc\";0,1=b;");
    // An opening quote that never closes is ordinary text.
    CHECK(Cells(PlanPaste("\"abc\tb", {0, 0}, nullptr, kRows, kCols)) == "0,0=\"abc;0,1=b;");
}

TEST_CASE("PlanPaste: the formula guard a grid copy adds is removed") {
    CHECK(Cells(PlanPaste("'=SUM(A1)\t'-2w\t'tis", {0, 0}, nullptr, kRows, kCols)) == "0,0==SUM(A1);0,1=-2w;0,2='tis;");
}
