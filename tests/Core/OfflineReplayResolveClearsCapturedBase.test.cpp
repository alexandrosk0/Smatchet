// Regression test for finding DR5(c): ResolveFieldEditConflict must clear the
// has_original_value presence flag alongside original_value. Before the fix a resolved
// scalar conflict left has_original_value = 1 over a now-NULL original_value, so the row
// still looked like it carried a CAPTURED base. The offline replay conflict gate keys on
// that presence flag (HasOriginalValue), so ServerMovedFromCapturedBase would re-suspend
// the row on every tick — an infinite resolve / re-conflict loop where the edit never
// applies. This exercises the pure SQLite path through the in-memory LocalCacheManager,
// reusing SqliteMemFixture like the sibling LocalCacheManager tests.

#include "../support/SqliteMemFixture.h"

#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

using smatchet_tests::SqliteMemFixture;

TEST_CASE("LocalCacheManager: ResolveFieldEditConflict clears has_original_value for a blank captured base" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    // A genuinely BLANK captured scalar base: hasOriginalValue = true, originalValue empty.
    // This is the exact shape that latched the resolve/re-conflict loop — the presence flag
    // outlived the value.
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("Jira", "ABC-1", "status", "{\"status\":\"mine\"}",
                                                              std::string(), std::string(), true);

    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].HasOriginalValue);
    REQUIRE(rows[0].OriginalValue.empty());

    fix.Ref().MarkFieldEditConflict(id, "{\"base\":\"\",\"mine\":\"mine\",\"theirs\":\"srv\"}");
    fix.Ref().ResolveFieldEditConflict(id, "{\"status\":\"resolved\"}");

    rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows[0].HasMergeConflict);
    // The core assertion: the presence flag is reset so the replay gate no longer treats the
    // row as carrying a captured base, breaking the re-suspend loop.
    CHECK_FALSE(rows[0].HasOriginalValue);
    CHECK(rows[0].OriginalValue.empty());
    CHECK(rows[0].ConflictContextJson.empty());
    CHECK(rows[0].FieldsPayloadJson == "{\"status\":\"resolved\"}");
}

TEST_CASE("LocalCacheManager: ResolveFieldEditConflict clears has_original_value for a non-blank captured base" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().EnqueuePendingFieldEdit("Jira", "ABC-2", "priority", "{\"priority\":\"High\"}",
                                                              std::string(), "Low", true);

    auto rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].HasOriginalValue);
    CHECK(rows[0].OriginalValue == "Low");

    fix.Ref().MarkFieldEditConflict(id, "{\"base\":\"Low\",\"mine\":\"High\",\"theirs\":\"Medium\"}");
    fix.Ref().ResolveFieldEditConflict(id, "{\"priority\":\"Medium\"}");

    rows = fix.Ref().LoadPendingFieldEdits();
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows[0].HasOriginalValue);
    CHECK(rows[0].OriginalValue.empty());
    CHECK(rows[0].FieldsPayloadJson == "{\"priority\":\"Medium\"}");
}
