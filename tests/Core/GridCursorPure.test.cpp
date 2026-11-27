// Bucket-A doctest for the grid's cell-cursor math (Ui/GridCursorPure.h): every move at the edges, Tab wrap, the
// skip-read-only sweep of a commit-and-move, and range extend. Header-only, no ImGui.

#include "Ui/GridCursorPure.h"

#include <doctest/doctest.h>

using smatchet::grid::cursor::CellPos;
using smatchet::grid::cursor::Extend;
using smatchet::grid::cursor::Move;
using smatchet::grid::cursor::Range;
using smatchet::grid::cursor::Step;
using smatchet::grid::cursor::StepToEditable;

namespace {
constexpr int kRows = 5;
constexpr int kCols = 4;
CellPos S(CellPos from, Move m, int page = 3) { return Step(from, m, kRows, kCols, page); }
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
