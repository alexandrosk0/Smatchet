// Bucket-A doctest for the pure ticket-grid column-order predicates extracted from
// SmatchetActiveProjectGridTable.cpp (GridColumnOrderPure.h). No ImGui — the two
// accessors are lambdas over plain vectors, exactly the shape the grid TU supplies
// over ImGuiTable::Columns[n].DisplayOrder and ImGuiTable::DisplayOrderToIndex.

#include "GridColumnOrderPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

// Mirrors the grid call site: displayOrder[n] is column n's visual slot, orderToIndex[o]
// is the logical column ImGui places at slot o.
int FirstMismatch(const std::vector<int>& displayOrder, const std::vector<int>& orderToIndex) {
    return GridFirstDisplayOrderMismatch(static_cast<int>(displayOrder.size()),
                                         [&displayOrder](int n) { return displayOrder[static_cast<size_t>(n)]; },
                                         [&orderToIndex](int o) { return orderToIndex[static_cast<size_t>(o)]; });
}

} // namespace

TEST_CASE("GridFirstDisplayOrderMismatch accepts a consistent map") {
    SUBCASE("identity — a freshly built table") {
        CHECK(FirstMismatch({0, 1, 2, 3}, {0, 1, 2, 3}) == -1);
    }
    SUBCASE("a committed drag — the two arrays are mutual inverses") {
        // Column 2 dragged to the front: orders are {1,2,0,3}, so slot 0 holds column 2.
        CHECK(FirstMismatch({1, 2, 0, 3}, {2, 0, 1, 3}) == -1);
    }
    SUBCASE("a zero-column table is trivially consistent") {
        CHECK(FirstMismatch({}, {}) == -1);
    }
}

TEST_CASE("GridFirstDisplayOrderMismatch rejects the column-count-change desync") {
    SUBCASE("the map was seeded with the display orders instead of their inverse") {
        // What ImGui's preserve-columns path leaves behind for a NoSavedSettings table:
        // DisplayOrderToIndex[n] = Columns[n].DisplayOrder. Correct only while identity.
        const std::vector<int> displayOrder = {1, 2, 0, 3};
        CHECK(FirstMismatch(displayOrder, displayOrder) == 0);
    }
    SUBCASE("a drag over a corrupt map collided two columns on one slot") {
        // Columns 1 and 2 both claim slot 2, and slot 1 still points at the stale column.
        // Slot 2 resolves back to column 1, so column 1 round-trips and the collision only
        // shows up at its second claimant -- detection is what matters, not which index.
        CHECK(FirstMismatch({0, 2, 2, 3}, {0, 1, 1, 3}) == 2);
    }
    SUBCASE("a display order outside the column range is itself a mismatch") {
        CHECK(FirstMismatch({0, 1, 7, 3}, {0, 1, 2, 3}) == 2);
        CHECK(FirstMismatch({0, 1, -1, 3}, {0, 1, 2, 3}) == 2);
    }
    SUBCASE("the first bad column is reported, not a later one") {
        CHECK(FirstMismatch({0, 3, 2, 1}, {0, 1, 2, 3}) == 1);
    }
}

TEST_CASE("GridVisualColumnOrderIsPermutation guards the ColumnOrder writeback") {
    const std::vector<std::string> keys = {"field:summary", "field:status", "field:worklog"};

    SUBCASE("every key exactly once, in any order") {
        CHECK(GridVisualColumnOrderIsPermutation(keys, 3));
        CHECK(GridVisualColumnOrderIsPermutation({"field:worklog", "field:summary", "field:status"}, 3));
    }
    SUBCASE("a repeated key — the shape that baked a duplicate into the saved order") {
        CHECK_FALSE(GridVisualColumnOrderIsPermutation({"field:summary", "field:worklog", "field:worklog"}, 3));
    }
    SUBCASE("short — the map skipped a slot") {
        CHECK_FALSE(GridVisualColumnOrderIsPermutation({"field:summary", "field:status"}, 3));
    }
    SUBCASE("long — more entries than the column set has") {
        std::vector<std::string> tooMany = keys;
        tooMany.push_back("field:duedate");
        CHECK_FALSE(GridVisualColumnOrderIsPermutation(tooMany, 3));
    }
    SUBCASE("an empty order matches an empty column set only") {
        CHECK(GridVisualColumnOrderIsPermutation({}, 0));
        CHECK_FALSE(GridVisualColumnOrderIsPermutation({}, 3));
    }
}
