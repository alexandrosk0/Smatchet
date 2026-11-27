// Bucket-A doctest for the pure helpers extracted from LuaConsolePlugin::OnDraw
// during the function-size decomposition. These exercise the ImGui-free
// selection-id and action-name-join logic in isolation; the surrounding panel
// layout belongs in bucket E (deferred).
//
// Anchors: Source/Plugins/LuaConsole/LuaConsolePlugin_detail.h.

#include "LuaConsolePlugin_detail.h"

#include "GridPane.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using lua_console_detail::CollectPaneSelectedTicketIds;
using lua_console_detail::CollectSelectedTicketIds;
using lua_console_detail::JoinRegisteredActionNames;

GridPane MakePane(const std::vector<std::string>& ids, std::vector<size_t> sorted, std::vector<size_t> shown) {
    std::vector<CachedTicket> tickets(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        tickets[i].id = ids[i];
    }
    GridPane pane;
    pane.ticketsSnapshot = std::make_shared<const std::vector<CachedTicket>>(std::move(tickets));
    pane.cachedSortedIndices = std::move(sorted);
    pane.filteredIndices = std::move(shown);
    return pane;
}

// Selects shown rows [firstRow, lastRow] of `pane` as a one-column cell range, the way the grid does.
void SelectRowRange(GridPane& pane, int firstRow, int lastRow) {
    static const std::string kField = "summary";
    const auto idAt = [&pane](int row) -> const std::string& {
        return (*pane.ticketsSnapshot)[pane.filteredIndices[static_cast<size_t>(row)]].id;
    };
    const auto fieldAt = [](int) -> const std::string& { return kField; };
    pane.gridState.RectSel.SetRange(firstRow, 0, lastRow, 0, static_cast<int>(pane.filteredIndices.size()), idAt, 1,
                                    fieldAt);
}

} // namespace

TEST_CASE("CollectPaneSelectedTicketIds maps a row pick through the shown rows when a search hides rows (#2499)") {
    GridPane pane = MakePane({"A", "B", "C", "D"}, {0, 1, 2, 3}, {2, 3});
    pane.gridState.RectSel.Rows = {0};
    const std::vector<std::string> out = CollectPaneSelectedTicketIds(pane);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "C");
}

TEST_CASE("CollectPaneSelectedTicketIds maps a cell range through the shown rows when hide-parents drops rows") {
    GridPane pane = MakePane({"A", "B", "C", "D"}, {3, 0, 2, 1}, {0, 1});
    SelectRowRange(pane, 0, 1);
    REQUIRE(pane.gridState.RectSel.Active);
    const std::vector<std::string> out = CollectPaneSelectedTicketIds(pane);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "A");
    CHECK(out[1] == "B");
}

TEST_CASE("CollectPaneSelectedTicketIds leaves out a row inside the range's box that is not a member") {
    GridPane pane = MakePane({"A", "B", "C"}, {0, 1, 2}, {0, 1, 2});
    SelectRowRange(pane, 0, 2);
    REQUIRE(pane.gridState.RectSel.Active);
    pane.gridState.RectSel.RowInRange[1] = 0;
    const std::vector<std::string> out = CollectPaneSelectedTicketIds(pane);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "A");
    CHECK(out[1] == "C");
}

TEST_CASE("CollectPaneSelectedTicketIds unions row picks and the range without duplicates") {
    GridPane pane = MakePane({"A", "B", "C", "D"}, {0, 1, 2, 3}, {1, 2, 3});
    pane.gridState.RectSel.Rows = {0, 2};
    SelectRowRange(pane, 0, 1);
    REQUIRE(pane.gridState.RectSel.Active);
    const std::vector<std::string> out = CollectPaneSelectedTicketIds(pane);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == "B");
    CHECK(out[1] == "C");
    CHECK(out[2] == "D");
}

TEST_CASE("CollectPaneSelectedTicketIds selects nothing when no rows are shown") {
    GridPane noneShown = MakePane({"A", "B"}, {0, 1}, {});
    noneShown.gridState.RectSel.Rows = {0};
    CHECK(CollectPaneSelectedTicketIds(noneShown).empty());

    GridPane oneShown = MakePane({"A", "B", "C"}, {0, 1, 2}, {2});
    oneShown.gridState.RectSel.Rows = {1, 5, -1};
    CHECK(CollectPaneSelectedTicketIds(oneShown).empty());

    GridPane noSnapshot;
    noSnapshot.filteredIndices = {0};
    noSnapshot.gridState.RectSel.Rows = {0};
    CHECK(CollectPaneSelectedTicketIds(noSnapshot).empty());
}

TEST_CASE("CollectSelectedTicketIds maps explicit rows through sorted indices") {
    const std::vector<size_t> sorted = {2, 0, 1};
    const std::vector<std::string> ids = {"A", "B", "C"};
    const std::vector<int> rows = {0, 2};
    const std::vector<std::string> out = CollectSelectedTicketIds(rows, false, 0, 0, sorted, ids);
    // row 0 -> sorted[0]=2 -> "C"; row 2 -> sorted[2]=1 -> "B"; sorted/unique.
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "B");
    CHECK(out[1] == "C");
}

TEST_CASE("CollectSelectedTicketIds includes the active rect range") {
    const std::vector<size_t> sorted = {0, 1, 2, 3};
    const std::vector<std::string> ids = {"id0", "id1", "id2", "id3"};
    const std::vector<int> rows = {0};
    const std::vector<std::string> out = CollectSelectedTicketIds(rows, true, 1, 2, sorted, ids);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == "id0");
    CHECK(out[1] == "id1");
    CHECK(out[2] == "id2");
}

TEST_CASE("CollectSelectedTicketIds de-duplicates overlapping explicit and rect selections") {
    const std::vector<size_t> sorted = {0, 1};
    const std::vector<std::string> ids = {"x", "y"};
    const std::vector<int> rows = {1};
    const std::vector<std::string> out = CollectSelectedTicketIds(rows, true, 0, 1, sorted, ids);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "x");
    CHECK(out[1] == "y");
}

TEST_CASE("CollectSelectedTicketIds skips out-of-range rows and indices") {
    const std::vector<size_t> sorted = {0, 99};
    const std::vector<std::string> ids = {"only"};
    const std::vector<int> rows = {-1, 5, 0, 1};
    const std::vector<std::string> out = CollectSelectedTicketIds(rows, false, 0, 0, sorted, ids);
    // row -1 and 5 are out of sorted range; row 1 -> sorted[1]=99 -> out of ids range.
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "only");
}

TEST_CASE("JoinRegisteredActionNames comma-joins names") {
    CHECK(JoinRegisteredActionNames({}) == "");
    CHECK(JoinRegisteredActionNames({"solo"}) == "solo");
    CHECK(JoinRegisteredActionNames({"a", "b", "c"}) == "a, b, c");
}
