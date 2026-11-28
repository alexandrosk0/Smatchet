// The one-time import of a legacy local cache into its planned location (Persistence/LocalCacheLocationMigration):
// ranking, the read-only guarantee, the init-lock protocol, the no-replace publish and its fallbacks.

#include "Persistence/LocalCacheLocationMigration.h"

#include <doctest/doctest.h>

#include <SQLiteCpp/SQLiteCpp.h>
#include <ghc/filesystem.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace smatchet::persistence;
namespace fs = ghc::filesystem;

namespace {

/// A fresh directory the test removes on exit.
struct TempDir {
    std::string Path;
    TempDir() {
        static std::atomic<int> counter{0};
        const fs::path base =
            fs::temp_directory_path() /
            ("smatchet_import_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(counter.fetch_add(1)));
        fs::create_directories(base);
        Path = base.string();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(Path, ec);
    }
    std::string File(const std::string& name) const { return Path + "/" + name; }
};

void MakeLegacyDb(const std::string& path, int pendingRows, int ticketRows) {
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE pending_creates (id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL)");
    db.exec("CREATE TABLE pending_field_edits (id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL)");
    db.exec("CREATE TABLE pending_actions (id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL)");
    db.exec("CREATE TABLE tickets_v2 (backend_key TEXT NOT NULL, id TEXT NOT NULL, PRIMARY KEY(backend_key, id))");
    for (int i = 0; i < pendingRows; ++i) {
        db.exec("INSERT INTO pending_creates (payload) VALUES ('{}')");
    }
    for (int i = 0; i < ticketRows; ++i) {
        db.exec("INSERT INTO tickets_v2 (backend_key, id) VALUES ('k', 'T-" + std::to_string(i) + "')");
    }
}

int CountIn(const std::string& path, const char* table) {
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement q(db, std::string("SELECT COUNT(*) FROM ") + table);
    return q.executeStep() ? q.getColumn(0).getInt() : -1;
}

std::string Slurp(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << content;
}

bool Exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::vector<std::string> Leftovers(const TempDir& dir, const std::string& targetName) {
    std::vector<std::string> names;
    for (fs::directory_iterator it(dir.Path); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (name != targetName && name.compare(0, targetName.size(), targetName) == 0) {
            names.push_back(name); // .init-lock, .import-<ts>-<pid>, sidecars
        }
    }
    return names;
}

} // namespace

TEST_CASE("import: the best legacy file wins (live pending rows first), and no legacy file is touched") {
    TempDir dir;
    const std::string target = dir.File("new/cache.sqlite");
    const std::string manyTickets = dir.File("cwd.sqlite");
    const std::string withPending = dir.File("asset.sqlite");
    MakeLegacyDb(manyTickets, 0, 50);
    MakeLegacyDb(withPending, 2, 1);
    const std::string beforeA = Slurp(manyTickets);
    const std::string beforeB = Slurp(withPending);
    const std::int64_t mtimeB = fs::last_write_time(withPending).time_since_epoch().count();

    const LegacyCacheImportResult r =
        ImportLegacyLocalCacheIfNeeded(target, {manyTickets, withPending}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK(r.OpenPath == target);
    CHECK(r.LegacyPath == withPending);
    REQUIRE(r.OtherCandidates.size() == 1);
    CHECK(r.OtherCandidates[0] == manyTickets);
    CHECK(CountIn(target, "pending_creates") == 2);
    CHECK(CountIn(target, "tickets_v2") == 1);
    CHECK(Slurp(manyTickets) == beforeA);
    CHECK(Slurp(withPending) == beforeB);
    CHECK(fs::last_write_time(withPending).time_since_epoch().count() == mtimeB);
    CHECK_FALSE(Exists(target + ".init-lock"));
}

TEST_CASE("import: rows still in the legacy file's WAL come across") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    {
        SQLite::Database holder(legacy, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        holder.exec("PRAGMA journal_mode=WAL");
        holder.exec("PRAGMA wal_autocheckpoint=0");
        holder.exec("CREATE TABLE pending_creates (id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL)");
        for (int i = 0; i < 5; ++i) {
            holder.exec("INSERT INTO pending_creates (payload) VALUES ('{}')");
        }
        // The rows live in legacy.sqlite-wal while this connection stays open.
        const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, LegacyCacheImportOptions());
        CHECK(r.Kind == LegacyCacheImportKind::Imported);
    }
    CHECK(CountIn(target, "pending_creates") == 5);
}

TEST_CASE("import: an existing target is never touched, and nothing else is created") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    WriteFile(target, "already here");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 1);

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::NotNeeded);
    CHECK(Slurp(target) == "already here");
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE("import: no legacy file at all leaves an empty target and no lock behind") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(
        target, {dir.File("nope1.sqlite"), dir.File("nope2.sqlite")}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::CreatedEmpty);
    REQUIRE(Exists(target));
    CHECK(fs::file_size(target) == 0);
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE("import: an empty legacy file loses to a populated one") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string empty = dir.File("empty.sqlite");
    const std::string populated = dir.File("populated.sqlite");
    MakeLegacyDb(empty, 0, 0);
    MakeLegacyDb(populated, 0, 3);
    const LegacyCacheImportResult r =
        ImportLegacyLocalCacheIfNeeded(target, {empty, populated}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK(r.LegacyPath == populated);
    CHECK(CountIn(target, "tickets_v2") == 3);
}

TEST_CASE("import: a corrupt legacy file is skipped, and only corrupt files mean LegacyUnreadable") {
    TempDir dir;
    const std::string garbage = dir.File("garbage.sqlite");
    WriteFile(garbage, std::string(4096, 'x'));
    {
        const std::string target = dir.File("only_garbage.sqlite");
        const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {garbage}, LegacyCacheImportOptions());
        CHECK(r.Kind == LegacyCacheImportKind::LegacyUnreadable);
        CHECK(r.OpenPath == target);
        CHECK(Exists(target));                           // created under the init-lock, never by the open
        CHECK_FALSE(Exists(target + ".init-lock"));      // and the lock is gone again
        CHECK(Slurp(garbage) == std::string(4096, 'x')); // never modified
    }
    {
        const std::string good = dir.File("good.sqlite");
        MakeLegacyDb(good, 1, 0);
        const std::string target = dir.File("with_good.sqlite");
        const LegacyCacheImportResult r =
            ImportLegacyLocalCacheIfNeeded(target, {garbage, good}, LegacyCacheImportOptions());
        CHECK(r.Kind == LegacyCacheImportKind::Imported);
        CHECK(r.LegacyPath == good);
    }
}

TEST_CASE("import: losing the publish race leaves the winner's file intact and removes the copy") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 1);
    LegacyCacheImportOptions options;
    options.BeforePublishForTest = [&]() { WriteFile(target, "the winner's marker"); };

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::NotNeeded);
    CHECK(Slurp(target) == "the winner's marker");
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE("import: a filesystem without an atomic no-replace rename falls back to a checked rename") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 2, 0);
    LegacyCacheImportOptions options;
    options.PublishForTest = [](const std::string&, const std::string&) { return ENOTSUP; };

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK(CountIn(target, "pending_creates") == 2);
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE("import: any other publish failure uses the legacy file this session and publishes nothing") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 0);
    LegacyCacheImportOptions options;
    options.PublishForTest = [](const std::string&, const std::string&) { return EACCES; };

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::UseLegacyThisSession);
    CHECK(r.OpenPath == legacy);
    CHECK_FALSE(Exists(target));
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE(
    "import: a live init-lock held by another importer defers: this session never borrows the file being copied") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 0);
    WriteFile(target + ".init-lock", "pid=1");
    LegacyCacheImportOptions options;
    options.BusyBudgetMs = 50;

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::Deferred);
    CHECK_FALSE(Exists(target));
    CHECK(Exists(target + ".init-lock")); // not ours: never removed
}

TEST_CASE("import: a stale init-lock from a crashed importer is reclaimed once") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 0);
    WriteFile(target + ".init-lock", "pid=1");
    fs::last_write_time(target + ".init-lock", std::chrono::system_clock::now() - std::chrono::hours(1));
    LegacyCacheImportOptions options;
    options.BusyBudgetMs = 50;
    options.StaleLockAge = std::chrono::seconds(120);

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK_FALSE(Exists(target + ".init-lock"));
}

TEST_CASE("import: a legacy file another process holds locked is used in place, not copied") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 0);
    SQLite::Database holder(legacy, SQLite::OPEN_READWRITE);
    holder.exec("BEGIN EXCLUSIVE");
    LegacyCacheImportOptions options;
    options.BusyBudgetMs = 50;

    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::UseLegacyThisSession);
    CHECK(r.OpenPath == legacy);
    CHECK_FALSE(Exists(target));
    CHECK(Leftovers(dir, "cache.sqlite").empty());
    holder.exec("ROLLBACK");
}

TEST_CASE("import: the target's directory is created, and an abort ends the import at once") {
    TempDir dir;
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 0);
    const std::string deep = dir.File("a/b/c/cache.sqlite");
    CHECK(ImportLegacyLocalCacheIfNeeded(deep, {legacy}, LegacyCacheImportOptions()).Kind ==
          LegacyCacheImportKind::Imported);
    CHECK(Exists(deep));

    std::atomic<bool> abort{true};
    LegacyCacheImportOptions aborted;
    aborted.Abort = &abort;
    const std::string other = dir.File("x/cache.sqlite");
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(other, {legacy}, aborted);
    CHECK(r.Kind == LegacyCacheImportKind::NotNeeded);
    CHECK_FALSE(Exists(other));
}

TEST_CASE("import: a candidate that is not a regular file is ignored and listed, never opened") {
    TempDir dir;
    // A directory where the file should be (a FIFO or device would block an open, so none of them is ever opened).
    const std::string notAFile = dir.File("not_a_file.sqlite");
    fs::create_directories(notAFile);
    const std::string good = dir.File("good.sqlite");
    MakeLegacyDb(good, 1, 1);
    const std::string target = dir.File("cache.sqlite");
    const LegacyCacheImportResult r =
        ImportLegacyLocalCacheIfNeeded(target, {notAFile, good}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK(r.LegacyPath == good);
    REQUIRE(r.OtherCandidates.size() == 1);
    CHECK(r.OtherCandidates[0] == notAFile);

    const std::string target2 = dir.File("cache2.sqlite");
    const LegacyCacheImportResult only =
        ImportLegacyLocalCacheIfNeeded(target2, {notAFile}, LegacyCacheImportOptions());
    CHECK(only.Kind ==
          LegacyCacheImportKind::CreatedEmpty); // nothing usable: a fresh start, not a borrow of a non-file
}

TEST_CASE("import: a copy holding a trigger, a view or a virtual table is not imported") {
    TempDir dir;
    const std::string implant = dir.File("implant.sqlite");
    {
        SQLite::Database db(implant, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec("CREATE TABLE tickets_v2 (backend_key TEXT, id TEXT)");
        db.exec("CREATE TABLE pending_field_edits (id INTEGER PRIMARY KEY, payload TEXT)");
        db.exec("CREATE TRIGGER t AFTER INSERT ON tickets_v2 BEGIN INSERT INTO pending_field_edits (payload) "
                "VALUES ('x'); END");
        db.exec("INSERT INTO tickets_v2 VALUES ('Jira@t#1', 'A-1')");
    }
    const std::string target = dir.File("cache.sqlite");
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {implant}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::LegacyUnreadable); // treated like a damaged file: start fresh
    CHECK(CountIn(target, "sqlite_master") == 0);             // the target is an empty file, not the implant
    CHECK(Leftovers(dir, "cache.sqlite").empty());
}

TEST_CASE("import: a candidate larger than the cap is ignored and listed") {
    TempDir dir;
    const std::string big = dir.File("big.sqlite");
    MakeLegacyDb(big, 1, 1);
    const std::string target = dir.File("cache.sqlite");
    LegacyCacheImportOptions options;
    options.MaxFileBytes = 1024; // far below any real database file
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {big}, options);
    CHECK(r.Kind == LegacyCacheImportKind::CreatedEmpty);
    CHECK(Exists(target));
}

TEST_CASE("import: no readable file and a lock that does not clear is Deferred, and the target is not created") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    WriteFile(target + ".init-lock", "pid=1 time=now\n");
    LegacyCacheImportOptions options;
    options.BusyBudgetMs = 100;
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {dir.File("missing.sqlite")}, options);
    CHECK(r.Kind == LegacyCacheImportKind::Deferred);
    CHECK_FALSE(Exists(target));
    CHECK(Exists(target + ".init-lock")); // another process's lock is never removed by a loser
}

TEST_CASE("import: a lock that is not ours is not removed when this import ends") {
    TempDir dir;
    const std::string target = dir.File("cache.sqlite");
    const std::string legacy = dir.File("legacy.sqlite");
    MakeLegacyDb(legacy, 1, 1);
    LegacyCacheImportOptions options;
    options.BeforePublishForTest = [&]() {
        // Simulates a stale-reclaim race that handed the lock file to another process mid-import.
        WriteFile(target + ".init-lock", "pid=2 time=other token=other\n");
    };
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {legacy}, options);
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK(Exists(target + ".init-lock"));
}

TEST_CASE("import: a hostile legacy file's view is never evaluated, and a crashed import's temp files are swept") {
    TempDir dir;
    const std::string hostile = dir.File("hostile.sqlite");
    {
        SQLite::Database db(hostile, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        // Counting this view would spin on an unbounded recursive query; only plain tables are ever counted.
        db.exec("CREATE VIEW pending_creates AS WITH RECURSIVE r(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM r) "
                "SELECT x FROM r");
        db.exec("CREATE TABLE tickets_v2 (backend_key TEXT, id TEXT)");
        db.exec("INSERT INTO tickets_v2 VALUES ('Jira@t#1', 'A-1')");
    }
    const LegacyCandidateInfo info = ProbeLegacyCandidate(hostile, 1000);
    CHECK(info.Error.empty());
    CHECK(info.PendingLive == 0);
    CHECK(info.TicketRows == 1);

    const std::string target = dir.File("cache.sqlite");
    WriteFile(target + ".import-1-2", "left by a crash");
    WriteFile(target + ".import-1-2-wal", "left by a crash");
    const std::string good = dir.File("good.sqlite");
    MakeLegacyDb(good, 1, 1);
    const LegacyCacheImportResult r = ImportLegacyLocalCacheIfNeeded(target, {good}, LegacyCacheImportOptions());
    CHECK(r.Kind == LegacyCacheImportKind::Imported);
    CHECK_FALSE(Exists(target + ".import-1-2"));
    CHECK_FALSE(Exists(target + ".import-1-2-wal"));
}
