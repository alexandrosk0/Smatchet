#include <doctest/doctest.h>

#include "StringUtil.h"

// TicketGridColumnsBuilder::Build canonicalizes each saved view field id via
// CanonicalizeGridFieldId (inline in StringUtil.h) so a Jira view saved before
// the #1291 comments dedupe renders the unified `comments` cell. We test the
// helper directly — linking TicketGridModel.cpp would pull heavy transitive deps.

TEST_CASE("CanonicalizeGridFieldId — legacy comment alias (#1291)") {
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

// TicketGridColumnsBuilder::Build now canonicalizes each saved view.ColumnOrder key
// the same way it built the column Key from view.Fields — via CanonicalGridColumnKey
// (inline in StringUtil.h): "field:" + CanonicalizeGridFieldId(trim(id)). Without it a
// pre-canonicalization order key (stray whitespace or legacy alias) misses the byKey
// lookup and the column drops to the appended tail (ticketgrid-columnorder-canon).
TEST_CASE("CanonicalGridColumnKey — ColumnOrder key matches the column Key") {
    SUBCASE("field key with stray whitespace trims to the canonical Key") {
        CHECK(CanonicalGridColumnKey("field:customfield_123") == "field:customfield_123");
        CHECK(CanonicalGridColumnKey("  field:customfield_123  ") == "field:customfield_123");
        CHECK(CanonicalGridColumnKey("field: customfield_123 ") == "field:customfield_123");
        CHECK(CanonicalGridColumnKey("\tfield:customfield_123\n") == "field:customfield_123");
    }

    SUBCASE("legacy comment alias folds onto comments inside the field key") {
        CHECK(CanonicalGridColumnKey("field:comment") == "field:comments");
        CHECK(CanonicalGridColumnKey("field: comment ") == "field:comments");
        CHECK(CanonicalGridColumnKey("field:comments") == "field:comments");
    }

    SUBCASE("non-field key is trimmed and passed through unchanged") {
        CHECK(CanonicalGridColumnKey("id") == "id");
        CHECK(CanonicalGridColumnKey("  id  ") == "id");
    }

    SUBCASE("empty and short keys do not over-read the field prefix") {
        CHECK(CanonicalGridColumnKey("") == "");
        CHECK(CanonicalGridColumnKey("fie") == "fie");
        CHECK(CanonicalGridColumnKey("field:") == "field:");
    }
}
