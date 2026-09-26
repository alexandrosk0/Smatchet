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

TEST_CASE("GridHeaderDragTargetOrder follows the mouse, not one neighbour per frame") {
    // Five 100px columns laid out at x = 0..500; centres 50, 150, 250, 350, 450.
    const std::vector<float> centers{50.0f, 150.0f, 250.0f, 350.0f, 450.0f};

    SUBCASE("mouse inside the held column's own cell keeps it in place") {
        CHECK(GridHeaderDragTargetOrder(centers, 1, 120.0f) == 1);
        CHECK(GridHeaderDragTargetOrder(centers, 1, 180.0f) == 1);
    }
    SUBCASE("mouse far to the right jumps straight to that slot in one step") {
        CHECK(GridHeaderDragTargetOrder(centers, 1, 470.0f) == 4);
        CHECK(GridHeaderDragTargetOrder(centers, 1, 360.0f) == 3);
    }
    SUBCASE("mouse far to the left jumps straight to that slot in one step") {
        CHECK(GridHeaderDragTargetOrder(centers, 4, 10.0f) == 0);
        CHECK(GridHeaderDragTargetOrder(centers, 4, 160.0f) == 2);
    }
    SUBCASE("a column only moves past a neighbour once the mouse crosses that neighbour's centre") {
        CHECK(GridHeaderDragTargetOrder(centers, 1, 240.0f) == 1);
        CHECK(GridHeaderDragTargetOrder(centers, 1, 260.0f) == 2);
    }
    SUBCASE("narrow column over a wide neighbour does not oscillate") {
        // H = 50px at 0..50, W = 300px at 50..350. Mouse at 210 has crossed W's centre (200).
        const std::vector<float> before{25.0f, 200.0f};
        CHECK(GridHeaderDragTargetOrder(before, 0, 210.0f) == 1);
        // After the move W sits at 0..300 (centre 150) and H at 300..350; the same mouse
        // position must keep H where it now is rather than send it back.
        const std::vector<float> after{150.0f, 325.0f};
        CHECK(GridHeaderDragTargetOrder(after, 1, 210.0f) == 1);
        // Moving back left only happens once the mouse crosses W's new centre.
        CHECK(GridHeaderDragTargetOrder(after, 1, 140.0f) == 0);
    }
    SUBCASE("out-of-range held slot is returned unchanged") {
        CHECK(GridHeaderDragTargetOrder(centers, -1, 100.0f) == -1);
        CHECK(GridHeaderDragTargetOrder(centers, 5, 100.0f) == 5);
        CHECK(GridHeaderDragTargetOrder({}, 0, 100.0f) == 0);
    }
}

TEST_CASE("GridHeaderDragAutoScrollSpeed") {
    // Strip 100..900, 40px edge bands, 500px/s at the edge.
    const float kMin = 100.0f;
    const float kMax = 900.0f;
    const float kZone = 40.0f;
    const float kSpeed = 500.0f;
    const float kGrab = 500.0f; // header grabbed mid-strip; every probe below moved away from it

    SUBCASE("no scroll away from the edges") {
        CHECK(GridHeaderDragAutoScrollSpeed(500.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMin + kZone, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMax - kZone, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(0.0f));
    }
    SUBCASE("ramps inside the band, full speed at the edge, left is negative") {
        CHECK(GridHeaderDragAutoScrollSpeed(kMin + 20.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(-250.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMin, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(-500.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMax - 20.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(250.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMax, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(500.0f));
    }
    SUBCASE("keeps speeding up past the edge, capped at four times the edge speed") {
        CHECK(GridHeaderDragAutoScrollSpeed(kMax + 40.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(1000.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMax + 5000.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(2000.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(kMin - 5000.0f, kGrab, kMin, kMax, kZone, kSpeed) == doctest::Approx(-2000.0f));
    }
    SUBCASE("a strip narrower than two bands shrinks the band instead of scrolling everywhere") {
        // 90px strip -> 30px bands; the middle 30px stays still.
        CHECK(GridHeaderDragAutoScrollSpeed(145.0f, 150.0f, 100.0f, 190.0f, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(100.0f, 150.0f, 100.0f, 190.0f, kZone, kSpeed) == doctest::Approx(-500.0f));
    }
    SUBCASE("only scrolls toward the side the drag moved to from the grab point") {
        // A header grabbed inside the right band and nudged LEFT must not scroll right (it
        // would slide the grid under the mouse and carry the column the wrong way).
        CHECK(GridHeaderDragAutoScrollSpeed(880.0f, 890.0f, kMin, kMax, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(895.0f, 890.0f, kMin, kMax, kZone, kSpeed) > 0.0f);
        CHECK(GridHeaderDragAutoScrollSpeed(120.0f, 110.0f, kMin, kMax, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(105.0f, 110.0f, kMin, kMax, kZone, kSpeed) < 0.0f);
    }
    SUBCASE("degenerate inputs never scroll") {
        CHECK(GridHeaderDragAutoScrollSpeed(0.0f, kGrab, 100.0f, 100.0f, kZone, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(0.0f, kGrab, kMin, kMax, 0.0f, kSpeed) == doctest::Approx(0.0f));
        CHECK(GridHeaderDragAutoScrollSpeed(0.0f, kGrab, kMin, kMax, kZone, 0.0f) == doctest::Approx(0.0f));
    }
}
