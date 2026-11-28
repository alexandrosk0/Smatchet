// The salvage after a corrupt-file rebuild: the queue tables of the quarantined copy are carried into the fresh cache
// where they can be read, what could not be is named in the recovery report, a request that was `sending` becomes
// `ambiguous`, and the report is stored so a later launch can still tell the user.

#include "LocalCacheManagerTestAccess.h"
#include "TempDbFile.h"

#include "PendingActionTypes.h"

#include <doctest/doctest.h>

#include <SQLiteCpp/SQLiteCpp.h>
#include <ghc/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr const char* kBk = "Jira@test#1";

void WriteBytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string ReadBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("salvage: queue rows come across, a sending action becomes ambiguous, and the record is stored") {
    smatchet_tests::TempDbFile oldFile("smatchet_salvage_old_");
    smatchet_tests::TempDbFile freshFile("smatchet_salvage_fresh_");
    std::int64_t actionId = 0;
    {
        LocalCacheManager old(oldFile.Path());
        old.EnqueuePendingCreate(kBk, "{\"project\":\"P\"}");
        old.EnqueuePendingCreate(kBk, "{\"project\":\"Q\"}");
        old.EnqueuePendingFieldEdit(kBk, "A-1", "summary", "{\"summary\":\"x\"}");
        actionId = old.EnqueuePendingAction(kBk, "comment", "A-1", "{\"body\":\"hi\"}", PendingActionState::kSending);
    }
    REQUIRE(actionId > 0);

    LocalCacheManager fresh(freshFile.Path());
    LocalCacheManagerTestAccess::Salvage(fresh, oldFile.Path(), oldFile.Path() + "-wal", "test damage");

    const smatchet::persistence::LocalCacheRecoveryReport& report = fresh.RecoveryReport();
    CHECK(report.Rebuilt);
    CHECK(report.SalvageAttempted);
    CHECK(report.FromThisLaunch);
    CHECK(report.QuarantinedPath == oldFile.Path());
    CHECK(report.Reason == "test damage");
    CHECK(report.IncompleteTables.empty());
    CHECK(report.SalvagedRows >= 4);
    CHECK(fresh.LoadPendingCreates().size() == 2);
    CHECK(fresh.LoadPendingFieldEdits().size() == 1);
    const std::vector<PendingActionRecord> actions = fresh.LoadPendingActions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].State == PendingActionState::kAmbiguous); // may have reached the tracker: checked before a resend

    smatchet::persistence::LocalCacheRecoveryReport stored;
    REQUIRE(fresh.ReadLastRecoveryReport(stored));
    CHECK(stored.Rebuilt);
    CHECK(stored.SalvagedRows == report.SalvagedRows);
    CHECK_FALSE(stored.FromThisLaunch); // never stored: only the launch that rebuilt says so
    CHECK_FALSE(ghc::filesystem::exists(oldFile.Path() + ".salvage")); // the working copy is gone
}

TEST_CASE("salvage: a table whose shape cannot take the rows is named, and the others still come across") {
    smatchet_tests::TempDbFile oldFile("smatchet_salvage_shape_");
    smatchet_tests::TempDbFile freshFile("smatchet_salvage_shape_fresh_");
    {
        SQLite::Database db(oldFile.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        // No `payload` column, which the new pending_creates requires: its rows cannot be filled in.
        db.exec("CREATE TABLE pending_creates (id INTEGER PRIMARY KEY, backend_key TEXT)");
        db.exec("INSERT INTO pending_creates (backend_key) VALUES ('Jira@test#1')");
        db.exec("CREATE TABLE cache_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
        db.exec("INSERT INTO cache_meta (key, value) VALUES ('some_flag_v1', '1')");
    }
    LocalCacheManager fresh(freshFile.Path());
    LocalCacheManagerTestAccess::Salvage(fresh, oldFile.Path(), oldFile.Path() + "-wal", "shape");
    const smatchet::persistence::LocalCacheRecoveryReport& report = fresh.RecoveryReport();
    CHECK(Has(report.IncompleteTables, "pending_creates"));
    CHECK_FALSE(Has(report.IncompleteTables, "cache_meta"));
    CHECK_FALSE(Has(report.IncompleteTables, "pending_actions")); // absent in the old file: nothing to carry over
    CHECK(fresh.HasCacheMetaFlag("some_flag_v1"));
    CHECK(fresh.LoadPendingCreates().empty());
}

TEST_CASE("rebuild: a file that is not a database is quarantined, says nothing could be salvaged, and the record "
          "survives the next launch") {
    smatchet_tests::TempDbFile file("smatchet_salvage_garbage_");
    const std::string garbage(8192, 'x');
    WriteBytes(file.Path(), garbage);
    std::string quarantined;
    {
        LocalCacheManager rebuilt(file.Path());
        const smatchet::persistence::LocalCacheRecoveryReport& report = rebuilt.RecoveryReport();
        REQUIRE(report.Rebuilt);
        CHECK(report.FromThisLaunch);
        CHECK(report.SalvageAttempted);
        CHECK(report.SalvagedRows == 0);
        CHECK(report.IncompleteTables.size() == 7); // every salvage table: the file could not be opened at all
        quarantined = report.QuarantinedPath;
        REQUIRE_FALSE(quarantined.empty());
        CHECK(ReadBytes(quarantined) == garbage);           // the damaged copy is kept untouched
        CHECK(rebuilt.EnqueuePendingCreate(kBk, "{}") > 0); // the new cache works
    }
    {
        LocalCacheManager again(file.Path());
        CHECK_FALSE(again.RecoveryReport().Rebuilt); // an ordinary open: nothing happened this launch
        smatchet::persistence::LocalCacheRecoveryReport last;
        REQUIRE(again.ReadLastRecoveryReport(last));
        CHECK(last.Rebuilt);
        CHECK_FALSE(last.FromThisLaunch);
        CHECK(last.QuarantinedPath == quarantined);
        CHECK(again.LoadPendingCreates().size() == 1);
    }
    std::remove(quarantined.c_str());
}

TEST_CASE("salvage: a missing quarantined file is reported, never thrown") {
    smatchet_tests::TempDbFile freshFile("smatchet_salvage_missing_");
    LocalCacheManager fresh(freshFile.Path());
    CHECK_NOTHROW(LocalCacheManagerTestAccess::Salvage(fresh, freshFile.Path() + ".nope",
                                                       freshFile.Path() + ".nope-wal", "gone"));
    CHECK(fresh.RecoveryReport().IncompleteTables.size() == 7);
}
