// Bucket-A doctest for the pure helpers behind the Key cell's "Copy N selected rows as CSV"
// item (GridCopyPure.h) and the shared RFC 4180 field escape (StringUtil.h CsvEscapeField).
// Header-only, no ImGui.

#include "StringUtil.h"
#include "Ui/GridCopyPure.h"

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <vector>

TEST_CASE("ResolveGridRowsToTicketIndices maps sort-order rows through sortedIdx, ascending") {
    const std::vector<std::size_t> sorted = {3, 0, 2, 1};
    const std::vector<std::size_t> got = ResolveGridRowsToTicketIndices({0, 2, 3}, sorted, 4);
    CHECK(got == std::vector<std::size_t>{3, 2, 1});
}

TEST_CASE("ResolveGridRowsToTicketIndices uses the identity mapping when sortedIdx is empty") {
    const std::vector<std::size_t> got = ResolveGridRowsToTicketIndices({1, 3}, {}, 5);
    CHECK(got == std::vector<std::size_t>{1, 3});
}

TEST_CASE("ResolveGridRowsToTicketIndices skips out-of-range rows and indices") {
    SUBCASE("negative rows") {
        CHECK(ResolveGridRowsToTicketIndices({-2, -1, 0}, {}, 3) == std::vector<std::size_t>{0});
    }
    SUBCASE("rows past sortedIdx") {
        const std::vector<std::size_t> sorted = {1, 0};
        CHECK(ResolveGridRowsToTicketIndices({0, 2, 7}, sorted, 2) == std::vector<std::size_t>{1});
    }
    SUBCASE("mapped index >= ticketCount") {
        const std::vector<std::size_t> sorted = {0, 9};
        CHECK(ResolveGridRowsToTicketIndices({0, 1}, sorted, 3) == std::vector<std::size_t>{0});
    }
    SUBCASE("identity mapping row >= ticketCount") {
        CHECK(ResolveGridRowsToTicketIndices({2, 3}, {}, 3) == std::vector<std::size_t>{2});
    }
    SUBCASE("no rows") { CHECK(ResolveGridRowsToTicketIndices({}, {}, 3).empty()); }
}

TEST_CASE("BuildCsvLine joins escaped cells with commas and a trailing newline") {
    SUBCASE("plain cells") { CHECK(BuildCsvLine({"a", "b", "c"}) == "a,b,c\n"); }
    SUBCASE("single cell") { CHECK(BuildCsvLine({"only"}) == "only\n"); }
    SUBCASE("empty cells stay empty") { CHECK(BuildCsvLine({"a", "", "b"}) == "a,,b\n"); }
    SUBCASE("comma is quoted") { CHECK(BuildCsvLine({"a,b", "c"}) == "\"a,b\",c\n"); }
    SUBCASE("quote is doubled") { CHECK(BuildCsvLine({"say \"hi\""}) == "\"say \"\"hi\"\"\"\n"); }
    SUBCASE("embedded LF and CR are quoted, not flattened") {
        CHECK(BuildCsvLine({"l1\nl2", "x\ry"}) == "\"l1\nl2\",\"x\ry\"\n");
    }
}

TEST_CASE("CsvEscapeField - quotes only when needed, doubles inner quotes") {
    CHECK(CsvEscapeField("plain") == "plain");
    CHECK(CsvEscapeField("a,b") == "\"a,b\"");
    CHECK(CsvEscapeField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(CsvEscapeField("line1\nline2") == "\"line1\nline2\"");
    CHECK(CsvEscapeField("cr\rhere") == "\"cr\rhere\"");
    CHECK(CsvEscapeField("") == "");
}
