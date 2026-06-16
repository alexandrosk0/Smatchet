// LocalCacheManager corruption-on-open characterization — Slice G Phase 1 (testing-surface.md §6
// Gap 6, plan docs/plans/active/slice-g-db-corruption.md).
//
// CHARACTERIZES today's behaviour, it does NOT fix it. The ctor's schema-init — the unguarded
// db.exec("CREATE TABLE IF NOT EXISTS tickets ...") at LocalCacheManager.cpp (~line 145) and every
// CREATE/migration after it — runs OUTSIDE the WAL-pragma try/catch. So opening a CORRUPT on-disk
// cache file throws SQLite::Exception straight out of the constructor: an uncaught startup crash
// (Quality Pillar 3 "never crash" gap). These cases PIN that throw so the planned Phase-2 hardening
// (silent rebuild-on-unreadable + a forensic <db>.corrupt-<ts> rename) lands as a deliberate test
// diff, never a silent behaviour change.
//
// Distinguishing case: a ZERO-byte file is NOT corrupt — SQLite treats an empty file as a brand-new
// database, so OPEN_CREATE succeeds and schema-init runs clean. Phase 2 must preserve that (an empty
// file is fresh, not garbage to nuke). The happy-reopen case is the control: a healthy file reopens
// and its data survives, proving the throw cases isolate corruption rather than a generally-broken
// on-disk path.
//
// File-backed by necessity: the throw is on the on-disk header read; ":memory:" is per-connection and
// cannot reach it (SqliteMemFixture owns the in-memory path). Uses the shared on-disk RAII helper
// tests/support/TempDbFile.h.

#include "../support/TempDbFile.h"

#include "LocalCacheManager.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>

using smatchet_tests::TempDbFile;

namespace {

// Single-backend key namespace for the ticket tables (multi-grid Slice 1b).
constexpr const char* kBk = "Jira";

// Overwrite the path with raw bytes (binary, truncating any prior content) so the next open sees
// exactly these bytes as the database file.
void WriteRawFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("LocalCacheManager: opening a garbage (non-SQLite) file throws out of the ctor "
          "(Pillar-3 crash characterization)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_corrupt_");
    // 200 bytes with no valid "SQLite format 3\0" magic — definitively not a database.
    WriteRawFile(tmp.Path(), std::string(200, '\xEF'));
    // TODAY: schema-init reads the header, hits SQLITE_NOTADB, and throws past the ctor. Phase 2
    // will flip this to a silent rebuild — when it does, this assertion is the intended diff.
    CHECK_THROWS_AS(LocalCacheManager(tmp.Path()), SQLite::Exception);
}

TEST_CASE("LocalCacheManager: opening a truncated SQLite header throws out of the ctor" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_trunc_");
    // Valid 16-byte magic only — far shorter than the 100-byte header SQLite requires, so the file
    // is non-empty-but-unreadable (distinct from the zero-byte fresh case below).
    WriteRawFile(tmp.Path(), std::string("SQLite format 3\0", 16));
    CHECK_THROWS_AS(LocalCacheManager(tmp.Path()), SQLite::Exception);
}

TEST_CASE("LocalCacheManager: opening a ZERO-byte file succeeds — SQLite treats empty as a fresh DB "
          "(empty != corrupt)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_empty_");
    WriteRawFile(tmp.Path(), std::string()); // create + truncate to 0 bytes
    // An empty file is a valid brand-new database to SQLite — the ctor must NOT throw, and the
    // freshly-initialised schema must be queryable. Phase 2's "rebuild on unreadable" must keep
    // this path throw-free (don't treat empty as corrupt).
    REQUIRE_NOTHROW(LocalCacheManager(tmp.Path()));
    LocalCacheManager mgr(tmp.Path());
    CHECK(mgr.GetAllTicketIds(kBk).empty());
}

TEST_CASE("LocalCacheManager: a healthy on-disk DB reopens cleanly and data persists across ctor "
          "cycles (control)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_reopen_");
    {
        LocalCacheManager mgr(tmp.Path()); // first open creates the file + full schema
        CachedTicket t;
        t.id = "ABC-1";
        t.fieldValues["summary"] = "persist me";
        mgr.SaveTicket(kBk, t);
    } // dtor closes the connection → WAL checkpoint on the last connection
    LocalCacheManager reopened(tmp.Path()); // reopen the same healthy file — must not throw
    CachedTicket got;
    REQUIRE(reopened.TryGetTicket(kBk, "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "persist me");
}
