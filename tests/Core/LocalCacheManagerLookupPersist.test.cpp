// LocalCacheManager's ILookupCache half, file-backed cases: rows survive closing and reopening the
// cache (the "remembered workflow survives a restart" guarantee, Quality Pillar 6) and a cache file
// created before `lookup_cache` existed gains the table on open (additive-only schema).
//
// File-backed by necessity: both cases need a second connection to see the first one's bytes, and
// ":memory:" is per-connection (SqliteMemFixture owns the in-memory cases in
// LocalCacheManagerLookup.test.cpp). Uses the shared on-disk RAII helper tests/support/TempDbFile.h,
// which removes the file and its WAL/SHM siblings. SmatchetTests only, like the other file-backed
// LocalCacheManager suites.

#include "../support/TempDbFile.h"

#include "ILookupCache.h"
#include "LocalCacheManager.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

using smatchet_tests::TempDbFile;

namespace {

constexpr const char* kKind = "workflow_transitions";

bool TableExists(SQLite::Database& db, const char* name) {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    q.bind(1, name);
    return q.executeStep() && q.getColumn(0).getInt() == 1;
}

} // namespace

TEST_SUITE("LocalCacheManagerLookupPersist") {

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
