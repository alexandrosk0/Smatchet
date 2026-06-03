// Bucket-A doctest for the pure helpers extracted from LuaConsolePlugin::OnDraw
// during the function-size decomposition. These exercise the ImGui-free
// selection-id and action-name-join logic in isolation; the surrounding panel
// layout belongs in bucket E (deferred).
//
// Anchors: Source/Plugins/LuaConsole/LuaConsolePlugin_detail.h.

#include "LuaConsolePlugin_detail.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using lua_console_detail::CollectSelectedTicketIds;
using lua_console_detail::JoinRegisteredActionNames;

} // namespace

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
