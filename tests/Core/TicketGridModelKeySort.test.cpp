#include <doctest/doctest.h>

#include "StringUtil.h"

// CompareFieldValuesForSort dispatches "key"/"issuekey" to
// CompareIssueKeyNatural (inline in StringUtil.h). We test the comparator
// directly — linking TicketGridModel.cpp would pull heavy transitive deps.

TEST_CASE("CompareIssueKeyNatural — natural order for issue keys") {
    SUBCASE("same project, numeric order") {
        CHECK(CompareIssueKeyNatural("PROJ-2", "PROJ-10"));
        CHECK_FALSE(CompareIssueKeyNatural("PROJ-10", "PROJ-2"));
        CHECK_FALSE(CompareIssueKeyNatural("PROJ-10", "PROJ-10"));
    }

    SUBCASE("three-digit vs two-digit") {
        CHECK(CompareIssueKeyNatural("PROJ-10", "PROJ-100"));
        CHECK_FALSE(CompareIssueKeyNatural("PROJ-100", "PROJ-10"));
    }

    SUBCASE("different projects sort by project prefix") {
        CHECK(CompareIssueKeyNatural("AAA-1", "BBB-1"));
        CHECK_FALSE(CompareIssueKeyNatural("BBB-1", "AAA-1"));
    }

    SUBCASE("same project same number") { CHECK_FALSE(CompareIssueKeyNatural("PROJ-42", "PROJ-42")); }
}
