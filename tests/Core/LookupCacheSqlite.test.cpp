// LocalCacheManager's ILookupCache half (LocalCacheManager_Lookup.cpp) — the `lookup_cache` table
// behind offline read-side lookups such as the remembered workflow (Quality Pillar 6). Covers the
// CRUD round trip, replace-on-upsert, backend/kind namespacing, persistence across a reopen (the
// "learned transitions survive a restart" guarantee) and an older cache file gaining the table.
// File-backed cases use a unique TempDbFile so two connections see the same data.

#include "../support/SqliteMemFixture.h"
#include "../support/TempDbFile.h"

#include "ILookupCache.h"
#include "LocalCacheManager.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet_tests::SqliteMemFixture;
using smatchet_tests::TempDbFile;

namespace {

constexpr const char* kKind = "workflow_transitions";

bool TableExists(SQLite::Database& db, const char* name) {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    q.bind(1, name);
    return q.executeStep() && q.getColumn(0).getInt() == 1;
}

} // namespace

TEST_SUITE("LookupCacheSqlite") {

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

    TEST_CASE("rows survive closing and reopening the cache file") {
        TempDbFile tmp;
        {
            LocalCacheManager mgr(tmp.Path());
            REQUIRE(mgr.UpsertLookup("Jira", kKind, "PROJ|bug|1", R"([{"id":"2","name":"In Progress"}])"));
        }
        LocalCacheManager reopened(tmp.Path());
        LookupCacheRow row;
        REQUIRE(reopened.TryGetLookup("Jira", kKind, "PROJ|bug|1", row));
        CHECK(row.PayloadJson == R"([{"id":"2","name":"In Progress"}])");
    }

    TEST_CASE("a cache file from before lookup_cache existed gains the table on open") {
        TempDbFile tmp;
        {
            LocalCacheManager mgr(tmp.Path());
            REQUIRE(mgr.UpsertLookup("Jira", kKind, "gone", "[]"));
        }
        {
            // Simulate the pre-change file: every other table present, lookup_cache absent.
            SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE);
            raw.exec("DROP TABLE lookup_cache");
            REQUIRE_FALSE(TableExists(raw, "lookup_cache"));
        }
        {
            LocalCacheManager mgr(tmp.Path());
            CHECK(mgr.LoadLookups("Jira", kKind).empty());
            REQUIRE(mgr.UpsertLookup("Jira", kKind, "PROJ|bug|1", "[]"));
        }
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READONLY);
        CHECK(TableExists(raw, "lookup_cache"));
    }

} // TEST_SUITE
