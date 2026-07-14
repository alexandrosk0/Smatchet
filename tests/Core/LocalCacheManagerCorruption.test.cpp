// LocalCacheManager corruption-on-open characterization — Slice G Phase 1 (testing-surface.md §6
// Gap 6, plan docs/plans/shipped/slice-g-db-corruption.md).
//
// Phase 2 (this slice): the ctor now SURVIVES a corrupt on-disk cache file. Before opening the
// `db` member, the ctor probes the file (QuarantineIfCorrupt in LocalCacheManager.cpp); an
// unreadable NON-EMPTY file is renamed aside to a forensic <db>.corrupt-<unixts> sibling and a
// fresh empty cache is rebuilt in its place — no uncaught SQLite::Exception, no startup crash
// (Quality Pillar 3 "never crash"). Cases 1-2 below assert that rebuild (no-throw + fresh-empty
// cache + a .corrupt-* forensic file); they were CHECK_THROWS_AS in Phase 1, and flipping them
// here is the deliberate, visible behaviour diff (never a silent change).
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
#include <ghc/filesystem.hpp>

#include <fstream>
#include <string>
#include <system_error>

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

// True iff a forensic quarantine sibling (`<dbPath>.corrupt-<ts>`) exists next to `dbPath` —
// the proof that Phase 2 moved the bad file aside rather than nuking it in place.
bool QuarantineSiblingExists(const std::string& dbPath) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    const fs::path p(dbPath);
    const fs::path dir = p.parent_path().empty() ? fs::path(".") : p.parent_path();
    const std::string prefix = p.filename().string() + ".corrupt-";
    if (!fs::exists(dir, ec) || ec) {
        return false;
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("LocalCacheManager: opening a garbage (non-SQLite) file rebuilds a fresh cache "
          "(Pillar-3 never-crash)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_corrupt_");
    // 200 bytes with no valid "SQLite format 3\0" magic — definitively not a database.
    WriteRawFile(tmp.Path(), std::string(200, '\xEF'));
    // Phase 2: schema-init no longer throws past the ctor — the unreadable file is quarantined to
    // a forensic <path>.corrupt-<ts> sibling and a fresh empty cache is rebuilt in its place.
    REQUIRE_NOTHROW(LocalCacheManager(tmp.Path()));
    CHECK(QuarantineSiblingExists(tmp.Path()));
    LocalCacheManager mgr(tmp.Path()); // reopens the rebuilt-fresh file cleanly
    CHECK(mgr.GetAllTicketIds(kBk).empty());
}

TEST_CASE("LocalCacheManager: opening a truncated SQLite header rebuilds a fresh cache" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp("smatchet_lcm_trunc_");
    // Valid 16-byte magic only — far shorter than the 100-byte header SQLite requires, so the file
    // is non-empty-but-unreadable (distinct from the zero-byte fresh case below).
    WriteRawFile(tmp.Path(), std::string("SQLite format 3\0", 16));
    REQUIRE_NOTHROW(LocalCacheManager(tmp.Path()));
    CHECK(QuarantineSiblingExists(tmp.Path()));
    LocalCacheManager mgr(tmp.Path());
    CHECK(mgr.GetAllTicketIds(kBk).empty());
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
