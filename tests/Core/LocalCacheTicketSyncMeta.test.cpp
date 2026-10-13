// LocalCacheManager's ticket_sync_meta table (docs/plans/active/delta-sync.md § Phase 2) - the SQLite
// specifics the dual-impl SyncCacheContract cannot see: the table is a separate additive table that
// survives a reopen, a ticket save does not reset it (tickets_v2 is INSERT OR REPLACE), a delete takes
// the meta with it in the same transaction, and the one-time site re-key carries it along.

#include "../support/SqliteMemFixture.h"
#include "../support/TempDbFile.h"

#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <string>
#include <utility>
#include <vector>

using smatchet_tests::SqliteMemFixture;
using smatchet_tests::TempDbFile;

namespace {

CachedTicket Row(const std::string& id, const std::string& updated, const std::string& summary = "s") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    t.fieldValues["updated"] = updated;
    return t;
}

TicketSyncMeta Meta(const std::string& updated, std::vector<std::string> fields, std::int64_t at = 100) {
    TicketSyncMeta m;
    m.RemoteUpdated = updated;
    m.Fields = std::move(fields);
    m.SyncedAtEpochSec = at;
    return m;
}

} // namespace

TEST_SUITE("LocalCacheTicketSyncMeta") {

    TEST_CASE("meta round-trips through a reopen of the cache file") {
        TempDbFile file;
        {
            LocalCacheManager cache(file.Path());
            cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T1")}, {Meta("T1", {"summary", "updated"}, 4242)});
        }
        LocalCacheManager reopened(file.Path());
        const auto meta = reopened.GetTicketSyncMeta("K");
        REQUIRE(meta.size() == 1u);
        const TicketSyncMeta& m = meta.at("A-1");
        CHECK(m.RemoteUpdated == "T1");
        CHECK(m.Fields == std::vector<std::string>{"summary", "updated"});
        CHECK(m.SyncedAtEpochSec == 4242);
    }

    TEST_CASE("the meta lives in its own table; a plain ticket save invalidates it") {
        TempDbFile file;
        LocalCacheManager cache(file.Path());
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T1")}, {Meta("T1", {"summary", "updated"})});
        cache.SaveTicket("K", Row("A-1", "T1", "edited locally")); // INSERT OR REPLACE on tickets_v2

        SQLite::Database raw(file.Path(), SQLite::OPEN_READONLY);
        CHECK(raw.tableExists("ticket_sync_meta"));
        SQLite::Statement q(raw, "SELECT COUNT(*) FROM ticket_sync_meta WHERE backend_key = 'K'");
        REQUIRE(q.executeStep());
        CHECK(q.getColumn(0).getInt() == 0); // SaveTicket rewrote the row without a version: meta dropped
        CHECK(cache.GetTicketSyncMeta("K").empty());
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T2")}, {Meta("T2", {"summary", "updated"})});
        CHECK(cache.GetTicketSyncMeta("K").count("A-1") == 1u);
    }

    TEST_CASE("SaveTicketKeepingSyncMeta rewrites the row and keeps its meta") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T1")}, {Meta("T1", {"summary", "updated"}, 777)});
        cache.SaveTicketKeepingSyncMeta("K", Row("A-1", "T1", "edited locally"));

        CachedTicket got;
        REQUIRE(cache.TryGetTicket("K", "A-1", got));
        CHECK(got.fieldValues["summary"] == "edited locally");
        const auto meta = cache.GetTicketSyncMeta("K");
        REQUIRE(meta.count("A-1") == 1u);
        CHECK(meta.at("A-1").RemoteUpdated == "T1");
        CHECK(meta.at("A-1").SyncedAtEpochSec == 777);
    }

    TEST_CASE("DeleteTicket and DeleteTickets remove the meta with the row") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T1"), Row("A-2", "T1"), Row("A-3", "T1")},
                                      {Meta("T1", {"summary"}), Meta("T1", {"summary"}), Meta("T1", {"summary"})});
        cache.DeleteTicket("K", "A-1");
        cache.DeleteTickets("K", {"A-2"});
        const auto meta = cache.GetTicketSyncMeta("K");
        CHECK(meta.size() == 1u);
        CHECK(meta.count("A-3") == 1u);
        CHECK(cache.GetAllTicketIds("K").size() == 1u);
    }

    TEST_CASE("a save without a stamp stores no meta and clears an old one") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "T1")}, {Meta("T1", {"summary"})});
        REQUIRE(cache.GetTicketSyncMeta("K").size() == 1u);
        cache.SaveTicketsWithSyncMeta("K", {Row("A-1", "")}, {TicketSyncMeta()});
        CHECK(cache.GetTicketSyncMeta("K").empty());
    }

    TEST_CASE("the one-time site re-key carries the meta to the new key") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTicketsWithSyncMeta("Jira", {Row("A-1", "T1")}, {Meta("T1", {"summary"})});
        cache.RunOneTimeCacheIdentityRekey({{"Jira", "Jira@acme#abc"}});
        CHECK(cache.GetTicketSyncMeta("Jira").empty());
        CHECK(cache.GetTicketSyncMeta("Jira@acme#abc").count("A-1") == 1u);
    }

    TEST_CASE("GetTicketsByIds returns only the rows that exist") {
        SqliteMemFixture fx;
        LocalCacheManager& cache = fx.Ref();
        cache.SaveTickets("K", {Row("A-1", "T1"), Row("A-2", "T1")});
        const std::vector<CachedTicket> rows = cache.GetTicketsByIds("K", {"A-2", "A-9", "A-1"});
        CHECK(rows.size() == 2u);
    }
}
