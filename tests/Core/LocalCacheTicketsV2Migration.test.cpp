// tickets_v2 backend-key namespacing — multi-grid Slice 1b (ADR-0018 decision 4,
// docs/plans/multi-grid-tabs-slice1-design.md § 3.5).
//
// Covers the three bucket-A cases the design mandates:
//   * One-time copy migration round-trip on a PRE-migration fixture DB: legacy v1 rows
//     (written by a raw SQLite connection, exactly as a pre-1b build left them) are copied
//     into the v2 family stamped with the configured backend's key, read back equal, and the
//     legacy tables remain present on disk (Persistence additive-only invariant).
//   * Namespaced disjointness: two backend keys' rows do not collide on the same numeric id
//     (the GitHub `#123` vs Plane collision class from the design § 3.4).
//   * Migration idempotence: the cache_meta flag prevents a double copy.
//
// The migration cases need a FILE-backed DB (two connections must see the same data: the raw
// legacy writer, then LocalCacheManager); `:memory:` is per-connection, so a unique temp file
// is created per case and removed (with its WAL/SHM siblings) on scope exit.

#include "../support/SqliteMemFixture.h"
#include "../support/TempDbFile.h"

#include "ConfigManager.h"
#include "LocalCacheManager.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>

using smatchet_tests::SqliteMemFixture;
using smatchet_tests::TempDbFile;

namespace {

// Write legacy v1 rows with a raw connection — byte-for-byte what a pre-1b build persisted.
void SeedLegacyTicket(SQLite::Database& db, const std::string& id, const std::string& summary,
                      const std::string& rich = std::string()) {
    {
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO tickets (id) VALUES (?)");
        ins.bind(1, id);
        ins.exec();
    }
    {
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO ticket_field_values (ticket_id, field_key, field_value) "
                                  "VALUES (?, 'summary', ?)");
        ins.bind(1, id);
        ins.bind(2, summary);
        ins.exec();
    }
    if (!rich.empty()) {
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO ticket_field_rich_values (ticket_id, field_key, field_value) "
                                  "VALUES (?, 'description', ?)");
        ins.bind(1, id);
        ins.bind(2, rich);
        ins.exec();
    }
}

void CreateLegacySchema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS tickets (id TEXT PRIMARY KEY)");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values ("
            "ticket_id TEXT NOT NULL, field_key TEXT NOT NULL, field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_rich_values ("
            "ticket_id TEXT NOT NULL, field_key TEXT NOT NULL, field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
}

int CountRows(SQLite::Database& db, const char* table) {
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    SQLite::Statement q(db, sql);
    REQUIRE(q.executeStep());
    return q.getColumn(0).getInt();
}

} // namespace

TEST_CASE("tickets_v2 migration: legacy rows copy into v2 stamped with the configured backend key, read-back equal, "
          "legacy tables retained" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        // Pre-migration fixture: ONLY the legacy schema + rows, exactly as a pre-1b build left it.
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreateLegacySchema(raw);
        SeedLegacyTicket(raw, "ABC-1", "first summary", "{\"version\":1,\"type\":\"doc\"}");
        SeedLegacyTicket(raw, "ABC-2", "second summary");
    }

    LocalCacheManager mgr(tmp.Path());
    const size_t copied = mgr.RunOneTimeTicketsV2CopyMigration("Jira");
    CHECK(copied == 2);

    // Read-back equality through the v2-backed read paths.
    CachedTicket got;
    REQUIRE(mgr.TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "first summary");
    CHECK(got.fieldRichValues["description"] == "{\"version\":1,\"type\":\"doc\"}");
    REQUIRE(mgr.TryGetTicket("Jira", "ABC-2", got));
    CHECK(got.fieldValues["summary"] == "second summary");
    CHECK(got.fieldRichValues.empty());
    CHECK(mgr.GetAllTicketIds("Jira").size() == 2);
    CHECK(mgr.GetAllTickets("Jira").size() == 2);

    // Rows are namespaced — a different backend key sees nothing.
    CHECK_FALSE(mgr.TryGetTicket("Plane", "ABC-1", got));
    CHECK(mgr.GetAllTicketIds("Plane").empty());

    // Legacy v1 tables are still present and untouched (additive-only invariant — no DROP).
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READONLY);
        CHECK(CountRows(raw, "tickets") == 2);
        CHECK(CountRows(raw, "ticket_field_values") == 2);
        CHECK(CountRows(raw, "ticket_field_rich_values") == 1);
    }
}

TEST_CASE("tickets_v2 migration: cache_meta flag makes the copy idempotent (no double-copy)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreateLegacySchema(raw);
        SeedLegacyTicket(raw, "ABC-1", "original");
    }

    {
        LocalCacheManager mgr(tmp.Path());
        CHECK(mgr.RunOneTimeTicketsV2CopyMigration("Jira") == 1);
        // Same-process re-run: flag short-circuits.
        CHECK(mgr.RunOneTimeTicketsV2CopyMigration("Jira") == 0);
    }

    // A legacy row written AFTER the migration (e.g. hand-edited DB) must NOT be swept in by a
    // later run — the flag persists across reopen.
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE);
        SeedLegacyTicket(raw, "LATE-1", "late row");
    }
    {
        LocalCacheManager mgr(tmp.Path());
        CHECK(mgr.RunOneTimeTicketsV2CopyMigration("Jira") == 0);
        CachedTicket got;
        CHECK_FALSE(mgr.TryGetTicket("Jira", "LATE-1", got));
        REQUIRE(mgr.TryGetTicket("Jira", "ABC-1", got));
        CHECK(got.fieldValues["summary"] == "original");
    }
}

TEST_CASE("tickets_v2 migration: empty backend key skips without consuming the one-time flag") {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreateLegacySchema(raw);
        SeedLegacyTicket(raw, "ABC-1", "kept");
    }
    LocalCacheManager mgr(tmp.Path());
    CHECK(mgr.RunOneTimeTicketsV2CopyMigration(std::string()) == 0);
    // The guard left the flag unset — a later properly-keyed call still migrates.
    CHECK(mgr.RunOneTimeTicketsV2CopyMigration("GitHub") == 1);
    CachedTicket got;
    REQUIRE(mgr.TryGetTicket("GitHub", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "kept");
}

TEST_CASE("tickets_v2 migration: stamps the RESOLVED config key, not a diverging Initialize parameter "
          "(CR-948-1 divergence: param 'Jira', cfg 'github')" *
          doctest::test_suite("[high-risk]")) {
    // AppController::Initialize(dbPath, backendType) can be called with a backendType parameter
    // that diverges from the config the live path resolves (embedded host ignores
    // options.BackendType in InitBackends; standalone Load(cli) vs Load()). The fix runs the
    // migration in InitBackends with the cfg-resolved key, so legacy rows land under the key
    // the live read path queries. AppController itself has no bucket-A seam (it drags the
    // UI/Lua/MCP link — see GridLiveContext.test.cpp), so this pins the contract at the
    // LocalCacheManager + NormalizeViewsBackendKey level: migrating with the RESOLVED key makes
    // rows visible to the live path's queries and invisible under the divergent parameter key.
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreateLegacySchema(raw);
        SeedLegacyTicket(raw, "ABC-1", "divergence row");
    }

    // The divergence: Initialize param "Jira" vs cfg.TrackerType "github".
    const std::string paramKey = ConfigManager::NormalizeViewsBackendKey("Jira");      // pre-fix stamp
    const std::string resolvedKey = ConfigManager::NormalizeViewsBackendKey("github"); // live path's key
    REQUIRE(paramKey != resolvedKey);

    LocalCacheManager mgr(tmp.Path());
    // Post-fix InitBackends passes the resolved key (focusedContext().CacheBackendKeyCopy()).
    CHECK(mgr.RunOneTimeTicketsV2CopyMigration(resolvedKey) == 1);

    // Migrated rows are exactly where the live path looks — and NOT under the parameter key.
    CachedTicket got;
    REQUIRE(mgr.TryGetTicket(resolvedKey, "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "divergence row");
    CHECK_FALSE(mgr.TryGetTicket(paramKey, "ABC-1", got));
    CHECK(mgr.GetAllTicketIds(paramKey).empty());
}

TEST_CASE("tickets_v2 migration: partial re-run still copies field/rich rows when ticket rows already exist "
          "(CR-948-5 per-table counts)") {
    // Simulate a crash between the tickets copy and the flag-set on a previous run: the ticket
    // row already exists in v2 (INSERT OR IGNORE copies 0 tickets) but its field/rich rows do
    // not. The migration must still copy them (return value stays "ticket rows copied" = 0).
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreateLegacySchema(raw);
        SeedLegacyTicket(raw, "ABC-1", "partial rerun", "{\"version\":1,\"type\":\"doc\"}");
    }
    LocalCacheManager mgr(tmp.Path()); // ctor creates the v2 tables
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE);
        raw.exec("INSERT INTO tickets_v2 (backend_key, id) VALUES ('Jira', 'ABC-1')");
    }
    CHECK(mgr.RunOneTimeTicketsV2CopyMigration("Jira") == 0); // 0 TICKET rows copied this pass
    CachedTicket got;
    REQUIRE(mgr.TryGetTicket("Jira", "ABC-1", got));
    CHECK(got.fieldValues["summary"] == "partial rerun"); // field row copied despite ticket pre-existing
    CHECK(got.fieldRichValues["description"] == "{\"version\":1,\"type\":\"doc\"}");
}

TEST_CASE("tickets_v2 namespacing: same numeric id under two backend keys stays disjoint "
          "(GitHub #123 vs Plane 123 collision class)" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;

    CachedTicket gh;
    gh.id = "123";
    gh.fieldValues["summary"] = "github issue";
    gh.fieldRichValues["description"] = "<p>gh</p>";
    CachedTicket plane;
    plane.id = "123";
    plane.fieldValues["summary"] = "plane work item";

    fix.Ref().SaveTicket("GitHub", gh);
    fix.Ref().SaveTicket("Plane", plane);

    // Same id, two namespaces, two independent rows.
    CachedTicket got;
    REQUIRE(fix.Ref().TryGetTicket("GitHub", "123", got));
    CHECK(got.fieldValues["summary"] == "github issue");
    CHECK(got.fieldRichValues["description"] == "<p>gh</p>");
    REQUIRE(fix.Ref().TryGetTicket("Plane", "123", got));
    CHECK(got.fieldValues["summary"] == "plane work item");
    CHECK(got.fieldRichValues.empty());

    CHECK(fix.Ref().GetAllTicketIds("GitHub").size() == 1);
    CHECK(fix.Ref().GetAllTicketIds("Plane").size() == 1);

    // A re-save under one key must not clobber the other's field snapshot.
    CachedTicket ghRevised;
    ghRevised.id = "123";
    ghRevised.fieldValues["summary"] = "github revised";
    fix.Ref().SaveTicket("GitHub", ghRevised);
    REQUIRE(fix.Ref().TryGetTicket("Plane", "123", got));
    CHECK(got.fieldValues["summary"] == "plane work item");

    // The stale-deletion hazard from the design § 3.4: pane A deleting "123" must not delete
    // pane B's row.
    fix.Ref().DeleteTicket("GitHub", "123");
    CHECK_FALSE(fix.Ref().TryGetTicket("GitHub", "123", got));
    REQUIRE(fix.Ref().TryGetTicket("Plane", "123", got));
    CHECK(got.fieldValues["summary"] == "plane work item");

    // Scoped streaming read sees only its own namespace.
    int planeCount = 0;
    fix.Ref().ForEachTicket("Plane", [&](CachedTicket&& t) {
        ++planeCount;
        CHECK(t.id == "123");
    });
    CHECK(planeCount == 1);
    int ghCount = 0;
    fix.Ref().ForEachTicket("GitHub", [&](CachedTicket&& t) {
        (void)t;
        ++ghCount;
    });
    CHECK(ghCount == 0);
}

// ============================================================================
// Pending-queue backend_key migrations — moved here from OfflineQueueBackendKey.test.cpp
// (ilocalcache-seam): these are LocalCacheManager IMPL tests (file-backed DB, raw legacy
// writer, off-interface RunOneTimePendingQueueBackendKeyStamp) and belong with the migration
// suite, keeping the service TU pure (ADR-0020). Reuses this TU's TempDbFile.
// ============================================================================

namespace {

// Write the PRE-1c queue schema with a raw connection — byte-for-byte what a pre-1c build
// persisted (no backend_key column anywhere).
void CreatePre1cQueueSchema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL, "
            "attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, "
            "payload TEXT NOT NULL, attempts INTEGER NOT NULL, last_error TEXT, "
            "created_at INTEGER NOT NULL, archived_at INTEGER NOT NULL, terminal_reason TEXT NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, issue_key TEXT NOT NULL, field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, original_rich_value TEXT, original_value TEXT, "
            "has_original_value INTEGER NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, "
            "issue_key TEXT NOT NULL, field_id TEXT NOT NULL, fields_payload_json TEXT NOT NULL, "
            "original_rich_value TEXT, original_value TEXT, has_original_value INTEGER NOT NULL DEFAULT 0, "
            "attempts INTEGER NOT NULL, last_error TEXT, created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, terminal_reason TEXT NOT NULL)");
}

void SeedLegacyQueueRows(SQLite::Database& db) {
    db.exec("INSERT INTO pending_creates (payload, attempts, last_error, created_at) VALUES ('{}', 0, '', 100)");
    db.exec("INSERT INTO pending_creates_dead (original_id, payload, attempts, last_error, created_at, archived_at, "
            "terminal_reason) VALUES (1, '{}', 5, 'err', 100, 200, 'max_attempts')");
    db.exec("INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, attempts, last_error, "
            "created_at) VALUES ('ABC-1', 'summary', '{}', 0, '', 100)");
    db.exec("INSERT INTO pending_field_edits_dead (original_id, issue_key, field_id, fields_payload_json, attempts, "
            "last_error, created_at, archived_at, terminal_reason) "
            "VALUES (1, 'ABC-2', 'summary', '{}', 5, 'err', 100, 200, 'max_attempts')");
}

bool TableHasColumn(SQLite::Database& db, const char* table, const char* col) {
    SQLite::Statement q(db, std::string("PRAGMA table_info(") + table + ")");
    while (q.executeStep()) {
        if (std::string(q.getColumn(1).getText()) == col)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("queue backend_key migration: pre-1c DB gains the column on open and re-opens cleanly (idempotent)" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreatePre1cQueueSchema(raw);
        SeedLegacyQueueRows(raw);
        REQUIRE_FALSE(TableHasColumn(raw, "pending_creates", "backend_key"));
    }
    {
        LocalCacheManager mgr(tmp.Path()); // first open: guarded ADD COLUMN fires on all 4 tables
        auto rows = mgr.LoadPendingCreates();
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().BackendKey.empty()); // legacy row: '' until the stamp migration runs
    }
    {
        LocalCacheManager mgr(tmp.Path()); // second open: column exists, ALTER skipped (no throw)
        CHECK(mgr.LoadPendingCreates().size() == 1);
    }
    SQLite::Database raw(tmp.Path(), SQLite::OPEN_READONLY);
    CHECK(TableHasColumn(raw, "pending_creates", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_creates_dead", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_field_edits", "backend_key"));
    CHECK(TableHasColumn(raw, "pending_field_edits_dead", "backend_key"));
}

TEST_CASE("queue backend_key stamp: legacy rows take the configured key once; flag persists across reopen" *
          doctest::test_suite("[high-risk]")) {
    TempDbFile tmp;
    {
        SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        CreatePre1cQueueSchema(raw);
        SeedLegacyQueueRows(raw);
    }
    {
        LocalCacheManager mgr(tmp.Path());
        // Empty key never consumes the one-time flag (corrupt/unwired-key guard).
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp(std::string()) == 0);
        // Real key stamps all 4 tables (1 row each in the fixture).
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("Jira") == 4);
        // Same-process re-run: flag short-circuits.
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("Jira") == 0);

        CHECK(mgr.LoadPendingCreates().front().BackendKey == "Jira");
        CHECK(mgr.LoadDeadPendingCreates().front().BackendKey == "Jira");
        CHECK(mgr.LoadPendingFieldEdits().front().BackendKey == "Jira");
        CHECK(mgr.LoadDeadPendingFieldEdits().front().BackendKey == "Jira");

        // A row enqueued AFTER the stamp keeps its own key.
        (void)mgr.EnqueuePendingCreate("Plane", "{}");
    }
    {
        // Reopen: the flag persists, so a different key never re-stamps (the Plane row would
        // otherwise be untouched anyway — it is non-empty — but the flag must short-circuit).
        LocalCacheManager mgr(tmp.Path());
        CHECK(mgr.RunOneTimePendingQueueBackendKeyStamp("GitHub") == 0);
        const auto rows = mgr.LoadPendingCreates();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].BackendKey == "Jira");
        CHECK(rows[1].BackendKey == "Plane");
    }
}
