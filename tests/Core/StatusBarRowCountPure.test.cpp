// Pure-logic coverage for the status bar's grid row count (StatusBarRowCountPure.h): when the
// count shows, "N rows" vs "X of N rows", and that a projection lagging a streaming sync is
// reported as the grid draws it rather than against the newer snapshot size.

#include "Ui/StatusBarRowCountPure.h"

#include <doctest/doctest.h>

using namespace StatusBarRowCountPure;

namespace {

Inputs OnePane(std::size_t loaded, std::size_t sorted, std::size_t filtered) {
    Inputs in;
    in.paneCount = 1;
    in.backendReachable = true;
    in.hasSnapshot = true;
    in.loaded = loaded;
    in.sorted = sorted;
    in.filtered = filtered;
    return in;
}

} // namespace

TEST_CASE("StatusBarRowCountPure: unfiltered single pane shows the total") {
    const Result r = Compute(OnePane(340, 340, 340));
    CHECK(r.show);
    CHECK_FALSE(r.Filtered());
    CHECK(r.visible == 340);
    CHECK(r.total == 340);
}

TEST_CASE("StatusBarRowCountPure: a narrowing search shows visible of total") {
    const Result r = Compute(OnePane(340, 340, 12));
    CHECK(r.Filtered());
    CHECK(r.visible == 12);
    CHECK(r.total == 340);
}

TEST_CASE("StatusBarRowCountPure: a search matching nothing reads 0 of total") {
    const Result r = Compute(OnePane(340, 340, 0));
    CHECK(r.Filtered());
    CHECK(r.visible == 0);
    CHECK(r.total == 340);
}

TEST_CASE("StatusBarRowCountPure: no projection yet shows the loaded total") {
    const Result r = Compute(OnePane(340, 0, 0));
    CHECK(r.show);
    CHECK_FALSE(r.Filtered());
    CHECK(r.total == 340);
}

TEST_CASE("StatusBarRowCountPure: a projection lagging a sync reports what the grid draws") {
    // 400 tickets arrived; the debounced rebuild still holds the 340-row projection, 12 visible.
    const Result r = Compute(OnePane(400, 340, 12));
    CHECK(r.Filtered());
    CHECK(r.visible == 12);
    CHECK(r.total == 340);
}

TEST_CASE("StatusBarRowCountPure: a projection from a larger snapshot is ignored") {
    // Loading / error states clear filteredIndices but skip the rebuild, so the old sort list remains.
    const Result emptied = Compute(OnePane(0, 340, 0));
    CHECK(emptied.show);
    CHECK_FALSE(emptied.Filtered());
    CHECK(emptied.total == 0);

    const Result shrunk = Compute(OnePane(100, 340, 12));
    CHECK_FALSE(shrunk.Filtered());
    CHECK(shrunk.total == 100);
}

TEST_CASE("StatusBarRowCountPure: hidden when the grid is ambiguous or not showing rows") {
    Inputs several = OnePane(340, 340, 340);
    several.paneCount = 3;
    CHECK_FALSE(Compute(several).show);
    several.hasFocusedPane = true;
    CHECK(Compute(several).show);

    Inputs none = OnePane(0, 0, 0);
    none.paneCount = 0;
    CHECK_FALSE(Compute(none).show);

    Inputs welcome = OnePane(340, 340, 0);
    welcome.backendReachable = false;
    CHECK_FALSE(Compute(welcome).show);

    Inputs noSnapshot = OnePane(0, 0, 0);
    noSnapshot.hasSnapshot = false;
    CHECK_FALSE(Compute(noSnapshot).show);
}
