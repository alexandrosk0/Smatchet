#include <doctest/doctest.h>

#include "StringUtil.h"

// TicketGridColumnsBuilder::Build canonicalizes each saved view field id via
// CanonicalizeGridFieldId (inline in StringUtil.h) so a Jira view saved before
// the #1018 comments dedupe renders the unified `comments` cell. We test the
// helper directly — linking TicketGridModel.cpp would pull heavy transitive deps.

TEST_CASE("CanonicalizeGridFieldId — legacy comment alias (#1018)") {
    SUBCASE("legacy singular folds onto unified id") {
        CHECK(CanonicalizeGridFieldId("comment") == "comments");
    }

    SUBCASE("already-unified id passes through") {
        CHECK(CanonicalizeGridFieldId("comments") == "comments");
    }

    SUBCASE("unrelated ids pass through unchanged") {
        CHECK(CanonicalizeGridFieldId("status") == "status");
        CHECK(CanonicalizeGridFieldId("") == "");
        CHECK(CanonicalizeGridFieldId("commentary") == "commentary");
    }
}
