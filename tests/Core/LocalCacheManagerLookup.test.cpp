// LocalCacheManager's ILookupCache half (LocalCacheManager_Lookup.cpp) — the `lookup_cache` table
// behind offline read-side lookups such as the remembered workflow (Quality Pillar 6). In-memory
// cases (SqliteMemFixture): the CRUD round trip, replace-on-upsert and backend/kind namespacing.
// The file-backed cases (restart persistence, an older cache file gaining the table) live in
// LocalCacheManagerLookupPersist.test.cpp.

#include "../support/SqliteMemFixture.h"

#include "ILookupCache.h"
#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet_tests::SqliteMemFixture;

namespace {

constexpr const char* kKind = "workflow_transitions";

} // namespace

TEST_SUITE("LocalCacheManagerLookup") {

    TEST_CASE("upsert, get, load and delete round-trip") {
        SqliteMemFixture fx;
        ILookupCache& cache = fx.Ref();

        REQUIRE(cache.UpsertLookup("Jira", kKind, "PROJ|bug|1", R"([{"id":"2","name":"In Progress"}])"));
        REQUIRE(cache.UpsertLookup("Jira", kKind, "PROJ|bug|2", R"([{"id":"3","name":"Done"}])"));

        LookupCacheRow row;
        REQUIRE(cache.TryGetLookup("Jira", kKind, "PROJ|bug|1", row));
        CHECK(row.CacheKey == "PROJ|bug|1");
        CHECK(row.PayloadJson == R"([{"id":"2","name":"In Progress"}])");
        CHECK(row.UpdatedAtEpochSec > 0);
        CHECK(cache.LoadLookups("Jira", kKind).size() == 2);

        CHECK(cache.DeleteLookup("Jira", kKind, "PROJ|bug|1"));
        CHECK_FALSE(cache.TryGetLookup("Jira", kKind, "PROJ|bug|1", row));
        const std::vector<LookupCacheRow> rest = cache.LoadLookups("Jira", kKind);
        REQUIRE(rest.size() == 1);
        CHECK(rest[0].CacheKey == "PROJ|bug|2");
        CHECK(cache.DeleteLookup("Jira", kKind, "missing")); // deleting nothing is not an error
    }

    TEST_CASE("upsert replaces the row for the same key") {
        SqliteMemFixture fx;
        ILookupCache& cache = fx.Ref();
        REQUIRE(cache.UpsertLookup("Jira", kKind, "PROJ|bug|1", "[1]"));
        REQUIRE(cache.UpsertLookup("Jira", kKind, "PROJ|bug|1", "[2]"));

        const std::vector<LookupCacheRow> rows = cache.LoadLookups("Jira", kKind);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].PayloadJson == "[2]");
    }

    TEST_CASE("backends and kinds are isolated namespaces") {
        SqliteMemFixture fx;
        ILookupCache& cache = fx.Ref();
        REQUIRE(cache.UpsertLookup("Jira", kKind, "K", "[\"jira\"]"));
        REQUIRE(cache.UpsertLookup("Plane", kKind, "K", "[\"plane\"]"));
        REQUIRE(cache.UpsertLookup("Jira", "other_kind", "K", "[\"other\"]"));

        LookupCacheRow row;
        REQUIRE(cache.TryGetLookup("Plane", kKind, "K", row));
        CHECK(row.PayloadJson == "[\"plane\"]");
        CHECK(cache.LoadLookups("Jira", kKind).size() == 1);
        CHECK(cache.LoadLookups("GitHub", kKind).empty());

        REQUIRE(cache.DeleteLookup("Jira", kKind, "K"));
        CHECK(cache.TryGetLookup("Plane", kKind, "K", row));
        CHECK(cache.TryGetLookup("Jira", "other_kind", "K", row));
    }

} // TEST_SUITE
