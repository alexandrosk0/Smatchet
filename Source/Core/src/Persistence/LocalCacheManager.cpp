#include "LocalCacheManager.h"

#include "IssueDraft.h"
#include "Logger.h"
#include "Persistence/SqliteOpenRecoveryPure.h"

#include <ghc/filesystem.hpp>
#include <sqlite3.h> // SQLITE_NOTADB / SQLITE_CORRUPT result codes (corrupt-file classification)

#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread> // std::this_thread::sleep_for — bounded backoff between schema-init retries (G2)

namespace {

namespace fs = ghc::filesystem;

/// Best-effort move of `from` -> `to` (never throws). Used to quarantine a corrupt
/// cache file + its WAL sidecars before rebuilding. A missing `from` is a silent no-op.
void QuarantinePath(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (!fs::exists(from, ec) || ec) {
        return;
    }
    fs::rename(from, to, ec);
    if (ec) {
        LOG_WARN("LocalCacheManager: could not quarantine '%s' -> '%s': %s", from.c_str(), to.c_str(),
                 ec.message().c_str());
    }
}

/// True if `table` has column `col` (identifier names only — must be fixed literals at call sites).
bool SqliteTableHasColumn(const SQLite::Database& db, const char* table, const char* col) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    SQLite::Statement q(db, sql);
    while (q.executeStep()) {
        if (std::strcmp(q.getColumn(1).getText(), col) == 0) {
            return true;
        }
    }
    return false;
}

/// Additive column migration that is safe under concurrent schema-init from a second connection.
/// The `table_info` guard is the fast path — an already-migrated cache runs no ALTER and throws no
/// exception. The catch closes the check-then-act TOCTOU window: two initializers opening the same
/// on-disk cache can both read the column as absent, and the loser's ALTER then fails with
/// "duplicate column name". That specific race is idempotent (the column the loser wanted now
/// exists), so it is swallowed; every other failure propagates untouched. `col`/`columnDef` are
/// fixed literals at all call sites (no user input reaches the SQL). Fixes the second contention
/// flake mode (schema-init race) that write-path busy-retry (#1894) did not cover.
void AddColumnIfMissing(SQLite::Database& db, const char* table, const char* col, const char* columnDef) {
    if (SqliteTableHasColumn(db, table, col)) {
        return;
    }
    const std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + col + " " + columnDef;
    try {
        db.exec(sql);
    } catch (const SQLite::Exception& ex) {
        if (smatchet::IsDuplicateColumnRace(ex.getErrorCode(), ex.what())) {
            LOG_WARN("LocalCacheManager: concurrent schema-init added %s.%s first (%s); continuing", table, col,
                     ex.what());
            return;
        }
        throw;
    }
}

// Creates / migrates the offline field-edit queue tables: `pending_field_edits` plus its
// dead-letter twin. Extracted from the constructor so the schema-init body stays under the
// function-size cap. All migrations are additive — CREATE IF NOT EXISTS plus guarded ADD COLUMN —
// per the Persistence forward-only invariant.
void InitFieldEditQueueSchema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "issue_key TEXT NOT NULL, "
            "field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, "
            "original_rich_value TEXT, "
            "original_value TEXT, "
            "has_original_value INTEGER NOT NULL DEFAULT 0, "
            "backend_key TEXT NOT NULL DEFAULT '', "
            "attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL)");
    AddColumnIfMissing(db, "pending_field_edits", "original_rich_value", "TEXT");
    // ADR-0016: additive scalar conflict base (display value). Twin of original_rich_value.
    AddColumnIfMissing(db, "pending_field_edits", "original_value", "TEXT");
    // ADR-0016: presence flag for the scalar base — distinguishes a genuinely BLANK captured base
    // ("" but present) from a legacy/no-base row, so a blank-field edit is still conflict-checked
    // instead of silently last-write-wins. Legacy rows default 0 (no base → last-write-wins).
    AddColumnIfMissing(db, "pending_field_edits", "has_original_value", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(db, "pending_field_edits", "has_merge_conflict", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(db, "pending_field_edits", "conflict_context_json", "TEXT");
    // Multi-grid Slice 1c (ADR-0018 decision 4): additive backend namespace for queue rows.
    // Legacy rows land as '' and are backfilled once by RunOneTimePendingQueueBackendKeyStamp.
    AddColumnIfMissing(db, "pending_field_edits", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "original_id INTEGER NOT NULL, "
            "issue_key TEXT NOT NULL, "
            "field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, "
            "original_rich_value TEXT, "
            "original_value TEXT, "
            "has_original_value INTEGER NOT NULL DEFAULT 0, "
            "backend_key TEXT NOT NULL DEFAULT '', "
            "attempts INTEGER NOT NULL, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, "
            "terminal_reason TEXT NOT NULL)");
    AddColumnIfMissing(db, "pending_field_edits_dead", "original_rich_value", "TEXT");
    // ADR-0016: additive scalar conflict base twin on the dead-letter table.
    AddColumnIfMissing(db, "pending_field_edits_dead", "original_value", "TEXT");
    // ADR-0016: presence-flag twin on the dead-letter table (mirrors pending_field_edits).
    AddColumnIfMissing(db, "pending_field_edits_dead", "has_original_value", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(db, "pending_field_edits_dead", "archived_at", "INTEGER NOT NULL DEFAULT 0");
    // Multi-grid Slice 1c: backend-namespace twin on the dead-letter table (carried over on
    // archive so the UI can attribute the row and a restore re-queues under the same backend).
    AddColumnIfMissing(db, "pending_field_edits_dead", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_field_edits_dead_archived_at "
            "ON pending_field_edits_dead(archived_at DESC)");
}

// Creates the backend-key-namespaced ticket table family (multi-grid Slice 1b, ADR-0018
// decision 4). The PK change (id → (backend_key, id)) cannot be done additively in place, so
// the v2 tables are a versioned step; the legacy `tickets` / `ticket_field_values` /
// `ticket_field_rich_values` tables are retained on disk unused (Persistence additive-only
// invariant — no DROP/RENAME). The PK prefix covers all per-backend lookups — no extra indices.
void InitTicketsV2Schema(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS tickets_v2 ("
            "backend_key TEXT NOT NULL, "
            "id TEXT NOT NULL, "
            "PRIMARY KEY(backend_key, id))");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values_v2 ("
            "backend_key TEXT NOT NULL, "
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(backend_key, ticket_id, field_key))");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_rich_values_v2 ("
            "backend_key TEXT NOT NULL, "
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(backend_key, ticket_id, field_key))");
}

// Run a write transaction with the SAME bounded backoff-and-retry the ctor uses for schema-init
// (Slice-G Phase 3 / G2): a SECOND connection writing the shared cache concurrently can surface a
// transient BUSY/LOCKED that busy_timeout does NOT absorb (a WAL snapshot / write-lock conflict is
// returned immediately, without waiting). Left unretried, that BUSY propagated out of SaveTicket as
// an uncaught throw — the write half of the "never-crash under contention" contract (Quality Pillar
// 3). On a transient code the SQLite::Transaction's dtor rolls the attempt back before the next one
// begins; ticket writes are idempotent upserts, so replay is safe. Non-transient errors and an
// exhausted budget propagate unchanged. Retry policy is the shared, unit-tested pure predicate
// (IsTransientBusyCode + BusyRetryBackoffMs), identical to the InitSchema loop.
//
// `stmtMutex` is taken PER ATTEMPT, wrapping the whole BEGIN…writeRows…COMMIT (both the cached
// prepared statements and the single-connection transaction are instance-serialized — SQLite
// forbids a nested transaction on one connection). It is released on scope exit BEFORE the backoff
// sleep, so a contending writer's retry never holds the mutex while sleeping and starves other
// statement executions on this instance. Templated on the write body (no std::function
// type-erasure; the closure inlines).
template <typename WriteFn>
void RunWriteTxnWithBusyRetry(SQLite::Database& db, std::mutex& stmtMutex, const char* opName, WriteFn&& writeRows) {
    constexpr int kMaxWriteAttempts = 5;
    for (int attempt = 0;; ++attempt) {
        try {
            std::lock_guard<std::mutex> lock(stmtMutex); // released on unwind BEFORE the sleep below
            SQLite::Transaction transaction(db);
            writeRows();
            transaction.commit();
            return;
        } catch (const SQLite::Exception& ex) {
            if (smatchet::IsTransientBusyCode(ex.getErrorCode()) && (attempt + 1) < kMaxWriteAttempts) {
                const int backoffMs = smatchet::BusyRetryBackoffMs(attempt);
                LOG_WARN("LocalCacheManager::%s contended (%s); retry %d/%d after %d ms", opName, ex.what(),
                         attempt + 2, kMaxWriteAttempts, backoffMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }
            LOG_ERROR("LocalCacheManager::%s failed err=%s", opName, ex.what());
            throw;
        }
    }
}

} // namespace

LocalCacheManager::LocalCacheManager(const std::string& dbPath)
    : dbPath_(dbPath), db(dbPath_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX) {
    LOG_INFO("LocalCacheManager: opening db path=%s", dbPath_.c_str());
    ApplyWalPragmas();
    // Schema-init is where a corrupt on-disk file OR live write-contention first surfaces (the
    // open is lazy). Three outcomes:
    //   * genuine on-disk corruption (NOTADB/CORRUPT) → quarantine + rebuild (Phase 2, #1352).
    //   * transient contention (BUSY/LOCKED) from a SECOND Smatchet opening the same shared
    //     cache → bounded backoff-and-retry (Phase 3 / G2), so a momentary lock window during
    //     startup does not crash the ctor (Pillar 3). busy_timeout already makes SQLite WAIT on
    //     lock acquisition; the retry covers the codes it returns immediately (e.g. a WAL
    //     snapshot conflict) and the case where the WAL pragma failed to arm the timeout.
    //   * anything else (CANTOPEN/IOERR permission/ENOENT race) → re-throw untouched: not
    //     corruption (never nuke a healthy cache) and not transient (retry would never clear it).
    // Empty/missing files are not corrupt — SQLite opens them as a fresh DB and InitSchema
    // succeeds, so they never reach this catch.
    constexpr int kMaxInitAttempts = 5;
    for (int attempt = 0;; ++attempt) {
        try {
            InitSchema();
            break;
        } catch (const SQLite::Exception& ex) {
            const int code = ex.getErrorCode();
            if (smatchet::IsRebuildableCorruptCode(code)) {
                RebuildFreshAfterCorruption(ex);
                break;
            }
            if (smatchet::ShouldRetryBusyInit(code, attempt, kMaxInitAttempts)) {
                const int backoffMs = smatchet::BusyRetryBackoffMs(attempt);
                LOG_WARN("LocalCacheManager: schema-init contended (%s); retry %d/%d after %d ms", ex.what(),
                         attempt + 2, kMaxInitAttempts, backoffMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }
            throw; // non-transient, or the retry budget is exhausted → propagate
        }
    }
    LOG_INFO("LocalCacheManager: schema ready");
}

void LocalCacheManager::RebuildFreshAfterCorruption(const SQLite::Exception& ex) {
    const std::string suffix = smatchet::MakeCorruptQuarantineSuffix(static_cast<long long>(std::time(nullptr)));
    LOG_ERROR("LocalCacheManager: cache file '%s' is corrupt (%s); quarantining to '%s' and "
              "rebuilding a fresh cache",
              dbPath_.c_str(), ex.what(), (dbPath_ + suffix).c_str());
    // Release the SQLite handle BEFORE renaming — Windows cannot move an open file. Move-assigning
    // an in-memory DB closes the on-disk connection (SQLiteCpp Database is move-assignable).
    db = SQLite::Database(":memory:");
    // Quarantine the corrupt main DB AND its WAL sidecars — a stale -wal/-shm against a fresh main
    // DB is itself a corruption hazard. Best-effort (QuarantinePath never throws).
    QuarantinePath(dbPath_, dbPath_ + suffix);
    QuarantinePath(dbPath_ + "-wal", dbPath_ + "-wal" + suffix);
    QuarantinePath(dbPath_ + "-shm", dbPath_ + "-shm" + suffix);
    // Reopen on the now-absent path → fresh empty DB, then re-init. A throw HERE is an
    // unrecoverable environment fault (not a corrupt file) and propagates.
    db = SQLite::Database(dbPath_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX);
    ApplyWalPragmas();
    InitSchema();
}

void LocalCacheManager::ApplyWalPragmas() {
    // Arm the busy-timeout FIRST (Slice-G Phase 3 / G2): the journal_mode=WAL pragma needs a
    // brief exclusive lock and can itself return SQLITE_BUSY under a concurrent open, so it must
    // run with the lock-wait already in effect; arming it first also guarantees the timeout is
    // set even if a later pragma throws. WAL improves crash safety and lets readers run while a
    // writer is active; synchronous=NORMAL is the recommended WAL pairing (fsync on checkpoint,
    // not every commit).
    try {
        db.setBusyTimeout(5000);
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA synchronous=NORMAL");
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager: failed to set WAL pragmas: %s", ex.what());
    }
}

void LocalCacheManager::InitSchema() {
    // Legacy v1 ticket tables — created for back-compat with the one-time v2 copy migration
    // (RunOneTimeTicketsV2CopyMigration reads them) and retained on disk unused afterwards
    // (Persistence additive-only invariant). All live reads/writes target the v2 family below.
    db.exec("CREATE TABLE IF NOT EXISTS tickets (id TEXT PRIMARY KEY)");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values ("
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
    // Parallel table for original rich-content payloads (ADF JSON / HTML) preserved alongside
    // stripped values so the long-text modal editor can round-trip without format loss. Empty
    // for tickets fetched before this column existed; absence means "fall back to stripped".
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_rich_values ("
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
    InitTicketsV2Schema(db);
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "payload TEXT NOT NULL, "
            "backend_key TEXT NOT NULL DEFAULT '', "
            "attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL)");
    // Multi-grid Slice 1c (ADR-0018 decision 4): additive backend namespace for queue rows.
    // Legacy rows land as '' and are backfilled once by RunOneTimePendingQueueBackendKeyStamp.
    AddColumnIfMissing(db, "pending_creates", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "original_id INTEGER NOT NULL, "
            "payload TEXT NOT NULL, "
            "backend_key TEXT NOT NULL DEFAULT '', "
            "attempts INTEGER NOT NULL, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, "
            "terminal_reason TEXT NOT NULL)");
    // Legacy DBs may predate `archived_at`; CREATE IF NOT EXISTS leaves an old table unchanged.
    // AddColumnIfMissing guards with PRAGMA table_info (no duplicate ADD COLUMN) and absorbs a
    // concurrent initializer winning the same migration.
    AddColumnIfMissing(db, "pending_creates_dead", "archived_at", "INTEGER NOT NULL DEFAULT 0");
    // Multi-grid Slice 1c: backend-namespace twin on the dead-letter table.
    AddColumnIfMissing(db, "pending_creates_dead", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_creates_dead_archived_at "
            "ON pending_creates_dead(archived_at DESC)");
    db.exec("CREATE TABLE IF NOT EXISTS cache_meta ("
            "key TEXT PRIMARY KEY, "
            "value TEXT NOT NULL)");
    InitFieldEditQueueSchema(db);
#if defined(SMATCHET_WITH_AI)
    // AI chat persistence (Phase 3 of ai-chat-claude-desktop-parity). Additive
    // table — old DBs auto-upgrade on first open. `pinned` flag is part of the
    // row schema so pinned-exempt trim can be expressed in a single DELETE.
    db.exec("CREATE TABLE IF NOT EXISTS ai_chat_messages ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "created_at INTEGER NOT NULL, "
            "role TEXT NOT NULL, "
            "content TEXT NOT NULL, "
            "pinned INTEGER NOT NULL DEFAULT 0)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_ai_chat_messages_id_desc ON ai_chat_messages(id DESC)");
#endif
}

SQLite::Statement& LocalCacheManager::stmt(std::unique_ptr<SQLite::Statement>& slot, const char* sql) {
    if (!slot) {
        // Freshly prepared statements have no bindings and no execution state to reset.
        slot = std::make_unique<SQLite::Statement>(db, sql);
        return *slot;
    }
    slot->reset();
    slot->clearBindings();
    return *slot;
}

// Write one ticket's rows using the cached prepared statements. Assumes `stmtMutex_` is held
// AND an enclosing SQLite::Transaction is active (SaveTicket / SaveTickets own the txn). Each
// single-bind statement is reset()+clearBindings() after exec so it can be rebound for the next
// ticket when called in a batch loop (SaveTickets).
void LocalCacheManager::writeTicketRows_(const std::string& backendKey, const CachedTicket& ticket) {
    auto& ticketUpsert =
        stmt(stmt_save_upsert_ticket_, "INSERT OR REPLACE INTO tickets_v2 (backend_key, id) VALUES (?, ?)");
    ticketUpsert.bind(1, backendKey);
    ticketUpsert.bind(2, ticket.id);
    ticketUpsert.exec();
    ticketUpsert.reset();
    ticketUpsert.clearBindings();

    // Keep selected field cache rows in sync with latest snapshot for this ticket.
    auto& deleteFields =
        stmt(stmt_save_delete_fields_, "DELETE FROM ticket_field_values_v2 WHERE backend_key = ? AND ticket_id = ?");
    deleteFields.bind(1, backendKey);
    deleteFields.bind(2, ticket.id);
    deleteFields.exec();
    deleteFields.reset();
    deleteFields.clearBindings();

    auto& fieldUpsert = stmt(stmt_save_insert_field_,
                             "INSERT OR REPLACE INTO ticket_field_values_v2 (backend_key, ticket_id, field_key, "
                             "field_value) VALUES (?, ?, ?, ?)");
    for (const auto& kv : ticket.fieldValues) {
        fieldUpsert.bind(1, backendKey);
        fieldUpsert.bind(2, ticket.id);
        fieldUpsert.bind(3, kv.first);
        fieldUpsert.bind(4, kv.second);
        fieldUpsert.exec();
        fieldUpsert.reset();
        fieldUpsert.clearBindings();
    }

    // Mirror the rich-value table.
    auto& deleteRich =
        stmt(stmt_save_delete_rich_, "DELETE FROM ticket_field_rich_values_v2 WHERE backend_key = ? AND ticket_id = ?");
    deleteRich.bind(1, backendKey);
    deleteRich.bind(2, ticket.id);
    deleteRich.exec();
    deleteRich.reset();
    deleteRich.clearBindings();

    auto& richUpsert = stmt(stmt_save_insert_rich_,
                            "INSERT OR REPLACE INTO ticket_field_rich_values_v2 (backend_key, ticket_id, field_key, "
                            "field_value) VALUES (?, ?, ?, ?)");
    for (const auto& kv : ticket.fieldRichValues) {
        if (kv.second.empty())
            continue;
        richUpsert.bind(1, backendKey);
        richUpsert.bind(2, ticket.id);
        richUpsert.bind(3, kv.first);
        richUpsert.bind(4, kv.second);
        richUpsert.exec();
        richUpsert.reset();
        richUpsert.clearBindings();
    }
}

void LocalCacheManager::SaveTicket(const std::string& backendKey, const CachedTicket& ticket) {
    // stmtMutex_ (cached-prepared-statement slots) is taken per-attempt INSIDE the retry helper so it
    // is not held across the backoff sleep.
    RunWriteTxnWithBusyRetry(db, stmtMutex_, "SaveTicket", [&] { writeTicketRows_(backendKey, ticket); });
}

// Phase 3(a) of docs/plans/shipped/memory-budget-and-lifetime-hardening.md: persist a whole slice
// of tickets in ONE transaction (the streaming-sync apply loop used to open a separate
// SQLite::Transaction per ticket — up to 20 commits/frame on the UI thread). Reuses the cached
// prepared statements across the batch.
void LocalCacheManager::SaveTickets(const std::string& backendKey, const std::vector<CachedTicket>& tickets) {
    if (tickets.empty()) {
        return;
    }
    // stmtMutex_ taken per-attempt inside the retry helper (not held across the backoff sleep).
    RunWriteTxnWithBusyRetry(db, stmtMutex_, "SaveTickets", [&] {
        for (const auto& ticket : tickets) {
            writeTicketRows_(backendKey, ticket);
        }
    });
}

bool LocalCacheManager::TryGetTicket(const std::string& backendKey, const std::string& ticketId, CachedTicket& out) {
    std::lock_guard<std::mutex> lock(stmtMutex_); // protects the cached-prepared-statement slots
    out = CachedTicket{};
    out.id = ticketId;
    try {
        auto& exists = stmt(stmt_get_exists_, "SELECT 1 FROM tickets_v2 WHERE backend_key = ? AND id = ? LIMIT 1");
        exists.bind(1, backendKey);
        exists.bind(2, ticketId);
        if (!exists.executeStep()) {
            return false;
        }
        auto& fieldQuery = stmt(stmt_get_fields_, "SELECT field_key, field_value FROM ticket_field_values_v2 "
                                                  "WHERE backend_key = ? AND ticket_id = ?");
        fieldQuery.bind(1, backendKey);
        fieldQuery.bind(2, ticketId);
        while (fieldQuery.executeStep()) {
            const std::string fieldKey = fieldQuery.getColumn(0).getText();
            out.fieldValues[fieldKey] =
                fieldQuery.getColumn(1).isNull() ? std::string() : std::string(fieldQuery.getColumn(1).getText());
        }
        auto& richQuery = stmt(stmt_get_rich_, "SELECT field_key, field_value FROM ticket_field_rich_values_v2 "
                                               "WHERE backend_key = ? AND ticket_id = ?");
        richQuery.bind(1, backendKey);
        richQuery.bind(2, ticketId);
        while (richQuery.executeStep()) {
            const std::string fieldKey = richQuery.getColumn(0).getText();
            out.fieldRichValues[fieldKey] =
                richQuery.getColumn(1).isNull() ? std::string() : std::string(richQuery.getColumn(1).getText());
        }
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::TryGetTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteTicket(const std::string& backendKey, const std::string& ticketId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement deleteFields(db,
                                       "DELETE FROM ticket_field_values_v2 WHERE backend_key = ? AND ticket_id = ?");
        deleteFields.bind(1, backendKey);
        deleteFields.bind(2, ticketId);
        deleteFields.exec();
        SQLite::Statement deleteRichFields(
            db, "DELETE FROM ticket_field_rich_values_v2 WHERE backend_key = ? AND ticket_id = ?");
        deleteRichFields.bind(1, backendKey);
        deleteRichFields.bind(2, ticketId);
        deleteRichFields.exec();
        SQLite::Statement deleteTicket(db, "DELETE FROM tickets_v2 WHERE backend_key = ? AND id = ?");
        deleteTicket.bind(1, backendKey);
        deleteTicket.bind(2, ticketId);
        deleteTicket.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

std::vector<CachedTicket> LocalCacheManager::GetAllTickets(const std::string& backendKey) {
    try {
        std::vector<CachedTicket> results;
        SQLite::Statement query(db, "SELECT id FROM tickets_v2 WHERE backend_key = ?");
        query.bind(1, backendKey);
        while (query.executeStep()) {
            CachedTicket ticket;
            ticket.id = query.getColumn(0).getText();
            results.push_back(ticket);
        }

        std::unordered_map<std::string, size_t> indexById;
        for (size_t i = 0; i < results.size(); ++i) {
            indexById[results[i].id] = i;
        }

        size_t orphanRows = 0;
        SQLite::Statement fieldQuery(
            db, "SELECT ticket_id, field_key, field_value FROM ticket_field_values_v2 WHERE backend_key = ?");
        fieldQuery.bind(1, backendKey);
        while (fieldQuery.executeStep()) {
            const std::string ticketId = fieldQuery.getColumn(0).getText();
            const auto it = indexById.find(ticketId);
            if (it == indexById.end()) {
                ++orphanRows;
                continue;
            }

            const std::string fieldKey = fieldQuery.getColumn(1).getText();
            const std::string fieldValue =
                fieldQuery.getColumn(2).isNull() ? std::string() : std::string(fieldQuery.getColumn(2).getText());
            results[it->second].fieldValues[fieldKey] = fieldValue;
        }
        if (orphanRows > 0) {
            LOG_WARN("LocalCacheManager::GetAllTickets ignored orphan field rows=%zu", orphanRows);
        }

        SQLite::Statement richQuery(
            db, "SELECT ticket_id, field_key, field_value FROM ticket_field_rich_values_v2 WHERE backend_key = ?");
        richQuery.bind(1, backendKey);
        while (richQuery.executeStep()) {
            const std::string ticketId = richQuery.getColumn(0).getText();
            const auto it = indexById.find(ticketId);
            if (it == indexById.end())
                continue;
            const std::string fieldKey = richQuery.getColumn(1).getText();
            const std::string fieldValue =
                richQuery.getColumn(2).isNull() ? std::string() : std::string(richQuery.getColumn(2).getText());
            results[it->second].fieldRichValues[fieldKey] = fieldValue;
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetAllTickets failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::ForEachTicket(const std::string& backendKey, const std::function<void(CachedTicket&&)>& fn) {
    try {
        // Join tickets with their field values and rich values in one pass per ticket to avoid
        // materialising the entire result set into memory (§4.3).
        SQLite::Statement q(db, "SELECT t.id, fv.field_key, fv.field_value, rv.field_key, rv.field_value "
                                "FROM tickets_v2 t "
                                "LEFT JOIN ticket_field_values_v2 fv "
                                "  ON fv.backend_key = t.backend_key AND fv.ticket_id = t.id "
                                "LEFT JOIN ticket_field_rich_values_v2 rv "
                                "  ON rv.backend_key = t.backend_key AND rv.ticket_id = t.id "
                                "  AND rv.field_key = fv.field_key "
                                "WHERE t.backend_key = ? "
                                "ORDER BY t.id");
        q.bind(1, backendKey);
        CachedTicket cur;
        while (q.executeStep()) {
            const std::string id = q.getColumn(0).getText();
            if (id != cur.id) {
                if (!cur.id.empty())
                    fn(std::move(cur));
                cur = CachedTicket{};
                cur.id = id;
            }
            if (!q.getColumn(1).isNull()) {
                const std::string fk = q.getColumn(1).getText();
                cur.fieldValues[fk] = q.getColumn(2).isNull() ? std::string() : q.getColumn(2).getText();
                if (!q.getColumn(3).isNull()) {
                    cur.fieldRichValues[fk] = q.getColumn(4).isNull() ? std::string() : q.getColumn(4).getText();
                }
            }
        }
        if (!cur.id.empty())
            fn(std::move(cur));
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ForEachTicket failed err=%s", ex.what());
        throw;
    }
}

std::vector<std::string> LocalCacheManager::GetAllTicketIds(const std::string& backendKey) {
    try {
        std::vector<std::string> results;
        SQLite::Statement query(db, "SELECT id FROM tickets_v2 WHERE backend_key = ?");
        query.bind(1, backendKey);
        while (query.executeStep()) {
            results.push_back(query.getColumn(0).getText());
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetAllTicketIds failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::RunOneTimeTicketsV2CopyMigration(const std::string& backendKey) {
    // Corrupt/unwired-key guard: never stamp rows with an empty namespace — leave the flag
    // unset so a later call with a real key still migrates.
    if (backendKey.empty()) {
        LOG_WARN("LocalCacheManager::RunOneTimeTicketsV2CopyMigration skipped: empty backendKey");
        return 0;
    }
    try {
        constexpr const char* kKey = "tickets_v2_copy_migration_v1";
        SQLite::Transaction transaction(db);
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, kKey);
        if (probe.executeStep()) {
            transaction.commit();
            return 0;
        }
        // INSERT OR IGNORE keeps the copy idempotent against a partially-written v2 state
        // (e.g. a crash between copy and flag-set on a previous run).
        // Per-table change counts (CR-948-5): `SQLite::Statement::exec()` returns the rows the
        // statement itself modified, so a partial re-run (e.g. ticket rows already copied by a
        // crashed earlier pass, field/rich rows not) still logs what THIS pass copied per table
        // instead of a misleading 0 from a single trailing `getChanges()`.
        SQLite::Statement copyTickets(db, "INSERT OR IGNORE INTO tickets_v2 (backend_key, id) "
                                          "SELECT ?, id FROM tickets");
        copyTickets.bind(1, backendKey);
        const int copiedTickets = copyTickets.exec();
        SQLite::Statement copyFields(db, "INSERT OR IGNORE INTO ticket_field_values_v2 "
                                         "(backend_key, ticket_id, field_key, field_value) "
                                         "SELECT ?, ticket_id, field_key, field_value FROM ticket_field_values");
        copyFields.bind(1, backendKey);
        const int copiedFields = copyFields.exec();
        SQLite::Statement copyRich(db, "INSERT OR IGNORE INTO ticket_field_rich_values_v2 "
                                       "(backend_key, ticket_id, field_key, field_value) "
                                       "SELECT ?, ticket_id, field_key, field_value FROM ticket_field_rich_values");
        copyRich.bind(1, backendKey);
        const int copiedRich = copyRich.exec();
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        ins.bind(1, kKey);
        ins.exec();
        transaction.commit();
        if (copiedTickets > 0 || copiedFields > 0 || copiedRich > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimeTicketsV2CopyMigration copied tickets=%d field_rows=%d "
                     "rich_rows=%d backend_key=%s",
                     copiedTickets, copiedFields, copiedRich, backendKey.c_str());
        }
        return copiedTickets > 0 ? static_cast<size_t>(copiedTickets) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimeTicketsV2CopyMigration failed backend_key=%s err=%s",
                  backendKey.c_str(), ex.what());
        throw;
    }
}

std::int64_t LocalCacheManager::EnqueuePendingCreate(const std::string& backendKey, const std::string& payload) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(db, "INSERT INTO pending_creates (payload, backend_key, attempts, last_error, "
                                     "created_at) VALUES (?, ?, 0, '', ?)");
        insert.bind(1, payload);
        insert.bind(2, backendKey);
        insert.bind(3, epoch);
        insert.exec();
        return db.getLastInsertRowid();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::EnqueuePendingCreate failed err=%s", ex.what());
        throw;
    }
}

std::vector<PendingCreate> LocalCacheManager::LoadPendingCreates() {
    try {
        std::vector<PendingCreate> results;
        SQLite::Statement query(db, "SELECT id, payload, attempts, last_error, created_at, backend_key "
                                    "FROM pending_creates ORDER BY id ASC");
        while (query.executeStep()) {
            PendingCreate pc;
            pc.Id = query.getColumn(0).getInt64();
            pc.Payload = query.getColumn(1).getText();
            pc.Attempts = query.getColumn(2).getInt();
            pc.LastError = query.getColumn(3).isNull() ? std::string() : query.getColumn(3).getText();
            pc.CreatedAtEpochSec = query.getColumn(4).getInt64();
            pc.BackendKey = query.getColumn(5).isNull() ? std::string() : query.getColumn(5).getText();
            results.push_back(std::move(pc));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadPendingCreates failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError) {
    try {
        SQLite::Statement update(db, "UPDATE pending_creates SET attempts = ?, last_error = ? WHERE id = ?");
        update.bind(1, attempts);
        update.bind(2, lastError);
        update.bind(3, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingCreate failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::UpdatePendingCreatePayload(std::int64_t id, const std::string& payload) {
    try {
        SQLite::Statement update(db, "UPDATE pending_creates SET payload = ? WHERE id = ?");
        update.bind(1, payload);
        update.bind(2, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingCreatePayload failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

bool LocalCacheManager::HasCacheMetaFlag(const std::string& key) {
    try {
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, key);
        return probe.executeStep();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::HasCacheMetaFlag failed key=%s err=%s", key.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::SetCacheMetaFlag(const std::string& key) {
    try {
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        ins.bind(1, key);
        ins.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::SetCacheMetaFlag failed key=%s err=%s", key.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::DeletePendingCreate(std::int64_t id) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE id = ?");
        del.bind(1, id);
        del.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeletePendingCreate failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ArchivePendingCreate(std::int64_t id, const std::string& terminalReason,
                                             const std::string& terminalError) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement select(
            db, "SELECT payload, attempts, last_error, created_at, backend_key FROM pending_creates WHERE id = ?");
        select.bind(1, id);
        if (!select.executeStep()) {
            throw std::runtime_error("pending_create row not found");
        }
        const std::string payload =
            select.getColumn(0).isNull() ? std::string() : std::string(select.getColumn(0).getText());
        const int attempts = select.getColumn(1).getInt();
        const std::string lastError =
            !terminalError.empty()
                ? terminalError
                : (select.getColumn(2).isNull() ? std::string() : std::string(select.getColumn(2).getText()));
        const std::int64_t createdAtEpochSec = select.getColumn(3).getInt64();
        const std::string backendKey =
            select.getColumn(4).isNull() ? std::string() : std::string(select.getColumn(4).getText());
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(
            db, "INSERT INTO pending_creates_dead "
                "(original_id, payload, backend_key, attempts, last_error, created_at, archived_at, terminal_reason) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, payload);
        insert.bind(3, backendKey);
        insert.bind(4, attempts);
        insert.bind(5, lastError);
        insert.bind(6, createdAtEpochSec);
        insert.bind(7, archivedAtEpochSec);
        insert.bind(8, terminalReason);
        insert.exec();

        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE id = ?");
        del.bind(1, id);
        del.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ArchivePendingCreate failed id=%lld reason=%s err=%s", static_cast<long long>(id),
                  terminalReason.c_str(), ex.what());
        throw;
    }
}

std::vector<DeadPendingCreate> LocalCacheManager::LoadDeadPendingCreates() {
    try {
        std::vector<DeadPendingCreate> results;
        SQLite::Statement query(db,
                                "SELECT dead_id, original_id, payload, attempts, last_error, created_at, archived_at, "
                                "terminal_reason, backend_key "
                                "FROM pending_creates_dead ORDER BY archived_at DESC, dead_id DESC");
        while (query.executeStep()) {
            DeadPendingCreate row;
            row.DeadId = query.getColumn(0).getInt64();
            row.OriginalId = query.getColumn(1).getInt64();
            row.Payload = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.Attempts = query.getColumn(3).getInt();
            row.LastError = query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.CreatedAtEpochSec = query.getColumn(5).getInt64();
            row.ArchivedAtEpochSec = query.getColumn(6).getInt64();
            row.TerminalReason =
                query.getColumn(7).isNull() ? std::string() : std::string(query.getColumn(7).getText());
            row.BackendKey = query.getColumn(8).isNull() ? std::string() : std::string(query.getColumn(8).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadDeadPendingCreates failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::GetDeadPendingCreateCount() {
    try {
        SQLite::Statement countQuery(db, "SELECT COUNT(*) FROM pending_creates_dead");
        if (!countQuery.executeStep()) {
            return 0;
        }
        const int count = countQuery.getColumn(0).getInt();
        return count > 0 ? static_cast<size_t>(count) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetDeadPendingCreateCount failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts() {
    try {
        constexpr const char* kKey = "legacy_drop_pending_ge_max_attempts_v1";
        SQLite::Transaction transaction(db);
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, kKey);
        if (probe.executeStep()) {
            transaction.commit();
            return 0;
        }
        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE attempts >= ?");
        del.bind(1, OfflineCreateQueue::kMaxReplayAttempts);
        del.exec();
        const int deleted = db.getChanges();
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        ins.bind(1, kKey);
        ins.exec();
        transaction.commit();
        if (deleted > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts dropped rows=%d", deleted);
        }
        return deleted > 0 ? static_cast<size_t>(deleted) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::RunOneTimePendingQueueBackendKeyStamp(const std::string& backendKey) {
    // Corrupt/unwired-key guard: never stamp rows with an empty namespace — leave the flag
    // unset so a later call with a real key still migrates. Mirrors the tickets_v2 copy guard.
    if (backendKey.empty()) {
        LOG_WARN("LocalCacheManager::RunOneTimePendingQueueBackendKeyStamp skipped: empty backendKey");
        return 0;
    }
    try {
        constexpr const char* kKey = "pending_queue_backend_key_stamp_v1";
        SQLite::Transaction transaction(db);
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, kKey);
        if (probe.executeStep()) {
            transaction.commit();
            return 0;
        }
        // Legacy rows were necessarily queued against the then-only configured backend, so
        // every un-stamped row ('' default from the additive ADD COLUMN) takes its key.
        static const char* const kTables[] = {"pending_creates", "pending_creates_dead", "pending_field_edits",
                                              "pending_field_edits_dead"};
        size_t stamped = 0;
        for (const char* table : kTables) {
            const std::string sql = std::string("UPDATE ") + table + " SET backend_key = ? WHERE backend_key = ''";
            SQLite::Statement upd(db, sql);
            upd.bind(1, backendKey);
            upd.exec();
            const int changed = db.getChanges();
            stamped += changed > 0 ? static_cast<size_t>(changed) : 0u;
        }
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        ins.bind(1, kKey);
        ins.exec();
        transaction.commit();
        if (stamped > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimePendingQueueBackendKeyStamp stamped rows=%zu backend_key=%s",
                     stamped, backendKey.c_str());
        }
        return stamped;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimePendingQueueBackendKeyStamp failed backend_key=%s err=%s",
                  backendKey.c_str(), ex.what());
        throw;
    }
}

bool LocalCacheManager::RestoreDeadPendingCreate(const std::int64_t originalPendingId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement sel(db, "SELECT dead_id, payload, backend_key FROM pending_creates_dead "
                                  "WHERE original_id = ? ORDER BY dead_id DESC LIMIT 1");
        sel.bind(1, originalPendingId);
        if (!sel.executeStep()) {
            transaction.commit();
            return false;
        }
        const std::int64_t deadId = sel.getColumn(0).getInt64();
        const std::string archivedPayload =
            sel.getColumn(1).isNull() ? std::string() : std::string(sel.getColumn(1).getText());
        // Fresh-create scrub applied to the archived payload INSIDE this transaction (the
        // service used to pre-load + scrub outside it — an exception there could skip the
        // scrub and restore a stale ExistingIssueKey verbatim). Unparseable payloads restore
        // verbatim; the replay tick's parse path terminally dead-letters real garbage.
        std::string parseErr;
        const std::string payload = IssueDraftHelpers::ScrubFreshCreatePayload(archivedPayload, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("LocalCacheManager::RestoreDeadPendingCreate original_id=%lld payload parse failed (%s) — "
                     "restoring verbatim",
                     static_cast<long long>(originalPendingId), parseErr.c_str());
        }
        // Restore re-queues under the SAME backend the row was originally queued against
        // (multi-grid Slice 1c) — never the currently-focused context's key.
        const std::string backendKey =
            sel.getColumn(2).isNull() ? std::string() : std::string(sel.getColumn(2).getText());
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement ins(db, "INSERT INTO pending_creates (payload, backend_key, attempts, last_error, "
                                  "created_at) VALUES (?, ?, 0, '', ?)");
        ins.bind(1, payload);
        ins.bind(2, backendKey);
        ins.bind(3, epoch);
        ins.exec();
        SQLite::Statement delDead(db, "DELETE FROM pending_creates_dead WHERE dead_id = ?");
        delDead.bind(1, deadId);
        delDead.exec();
        transaction.commit();
        LOG_INFO("LocalCacheManager::RestoreDeadPendingCreate original_id=%lld dead_id=%lld",
                 static_cast<long long>(originalPendingId), static_cast<long long>(deadId));
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RestoreDeadPendingCreate failed original_id=%lld err=%s",
                  static_cast<long long>(originalPendingId), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteDeadPendingCreate(const std::int64_t deadId) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_creates_dead WHERE dead_id = ?");
        del.bind(1, deadId);
        del.exec();
        if (db.getChanges() == 0) {
            LOG_WARN("LocalCacheManager::DeleteDeadPendingCreate: no row for dead_id=%lld",
                     static_cast<long long>(deadId));
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteDeadPendingCreate failed dead_id=%lld err=%s",
                  static_cast<long long>(deadId), ex.what());
        throw;
    }
}

std::int64_t LocalCacheManager::EnqueuePendingFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                                        const std::string& fieldId,
                                                        const std::string& fieldsPayloadJson,
                                                        const std::string& originalRichValue,
                                                        const std::string& originalValue, bool hasOriginalValue) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(db, "INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, "
                                     "original_rich_value, original_value, has_original_value, backend_key, attempts, "
                                     "last_error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0, '', ?)");
        insert.bind(1, issueKey);
        insert.bind(2, fieldId);
        insert.bind(3, fieldsPayloadJson);
        if (originalRichValue.empty())
            insert.bind(4); // NULL
        else
            insert.bind(4, originalRichValue);
        // original_value is stored NULL when blank to save space, but has_original_value records
        // whether a base was CAPTURED (presence) independent of its emptiness (ADR-0016).
        if (originalValue.empty())
            insert.bind(5); // NULL
        else
            insert.bind(5, originalValue);
        insert.bind(6, hasOriginalValue ? 1 : 0);
        insert.bind(7, backendKey);
        insert.bind(8, epoch);
        insert.exec();
        return db.getLastInsertRowid();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::EnqueuePendingFieldEdit failed err=%s", ex.what());
        throw;
    }
}

std::vector<PendingFieldEditRecord> LocalCacheManager::LoadPendingFieldEdits() {
    try {
        std::vector<PendingFieldEditRecord> results;
        SQLite::Statement query(db, "SELECT id, issue_key, field_id, fields_payload_json, "
                                    "COALESCE(original_rich_value, ''), "
                                    "COALESCE(has_merge_conflict, 0), COALESCE(conflict_context_json, ''), "
                                    "attempts, last_error, created_at, COALESCE(original_value, ''), "
                                    "COALESCE(has_original_value, 0), COALESCE(backend_key, '') "
                                    "FROM pending_field_edits ORDER BY id ASC");
        while (query.executeStep()) {
            PendingFieldEditRecord row;
            row.Id = query.getColumn(0).getInt64();
            row.IssueKey = query.getColumn(1).isNull() ? std::string() : std::string(query.getColumn(1).getText());
            row.FieldId = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.FieldsPayloadJson =
                query.getColumn(3).isNull() ? std::string() : std::string(query.getColumn(3).getText());
            row.OriginalRichValue =
                query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.HasMergeConflict = query.getColumn(5).getInt() != 0;
            row.ConflictContextJson =
                query.getColumn(6).isNull() ? std::string() : std::string(query.getColumn(6).getText());
            row.Attempts = query.getColumn(7).getInt();
            row.LastError = query.getColumn(8).isNull() ? std::string() : std::string(query.getColumn(8).getText());
            row.CreatedAtEpochSec = query.getColumn(9).getInt64();
            row.OriginalValue =
                query.getColumn(10).isNull() ? std::string() : std::string(query.getColumn(10).getText());
            row.HasOriginalValue = query.getColumn(11).getInt() != 0;
            row.BackendKey = query.getColumn(12).isNull() ? std::string() : std::string(query.getColumn(12).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadPendingFieldEdits failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError) {
    try {
        SQLite::Statement update(db, "UPDATE pending_field_edits SET attempts = ?, last_error = ? WHERE id = ?");
        update.bind(1, attempts);
        update.bind(2, lastError);
        update.bind(3, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingFieldEdit failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::MarkFieldEditConflict(std::int64_t id, const std::string& contextJson) {
    try {
        SQLite::Statement upd(
            db, "UPDATE pending_field_edits SET has_merge_conflict = 1, conflict_context_json = ? WHERE id = ?");
        upd.bind(1, contextJson);
        upd.bind(2, id);
        upd.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::MarkFieldEditConflict failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedPayloadJson) {
    try {
        SQLite::Statement upd(db,
                              "UPDATE pending_field_edits SET has_merge_conflict = 0, conflict_context_json = NULL, "
                              "original_rich_value = NULL, original_value = NULL, has_original_value = 0, "
                              "fields_payload_json = ? WHERE id = ?");
        upd.bind(1, resolvedPayloadJson);
        upd.bind(2, id);
        upd.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ResolveFieldEditConflict failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::DeletePendingFieldEdit(std::int64_t id) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_field_edits WHERE id = ?");
        del.bind(1, id);
        del.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeletePendingFieldEdit failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason,
                                                const std::string& terminalError) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement select(db, "SELECT issue_key, field_id, fields_payload_json, "
                                     "COALESCE(original_rich_value,''), attempts, last_error, created_at, "
                                     "COALESCE(original_value,''), COALESCE(has_original_value, 0), "
                                     "COALESCE(backend_key, '') FROM "
                                     "pending_field_edits WHERE id = ?");
        select.bind(1, id);
        if (!select.executeStep()) {
            throw std::runtime_error("pending_field_edits row not found");
        }
        const std::string issueKey =
            select.getColumn(0).isNull() ? std::string() : std::string(select.getColumn(0).getText());
        const std::string fieldId =
            select.getColumn(1).isNull() ? std::string() : std::string(select.getColumn(1).getText());
        const std::string payload =
            select.getColumn(2).isNull() ? std::string() : std::string(select.getColumn(2).getText());
        const std::string originalRichValue =
            select.getColumn(3).isNull() ? std::string() : std::string(select.getColumn(3).getText());
        const int attempts = select.getColumn(4).getInt();
        const std::string lastError =
            !terminalError.empty()
                ? terminalError
                : (select.getColumn(5).isNull() ? std::string() : std::string(select.getColumn(5).getText()));
        const std::int64_t createdAtEpochSec = select.getColumn(6).getInt64();
        const std::string originalValue =
            select.getColumn(7).isNull() ? std::string() : std::string(select.getColumn(7).getText());
        const bool hasOriginalValue = select.getColumn(8).getInt() != 0;
        const std::string backendKey =
            select.getColumn(9).isNull() ? std::string() : std::string(select.getColumn(9).getText());
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(db,
                                 "INSERT INTO pending_field_edits_dead "
                                 "(original_id, issue_key, field_id, fields_payload_json, original_rich_value, "
                                 "original_value, has_original_value, backend_key, attempts, last_error, created_at, "
                                 "archived_at, terminal_reason) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, issueKey);
        insert.bind(3, fieldId);
        insert.bind(4, payload);
        if (originalRichValue.empty())
            insert.bind(5);
        else
            insert.bind(5, originalRichValue);
        if (originalValue.empty())
            insert.bind(6);
        else
            insert.bind(6, originalValue);
        insert.bind(7, hasOriginalValue ? 1 : 0);
        insert.bind(8, backendKey);
        insert.bind(9, attempts);
        insert.bind(10, lastError);
        insert.bind(11, createdAtEpochSec);
        insert.bind(12, archivedAtEpochSec);
        insert.bind(13, terminalReason);
        insert.exec();

        SQLite::Statement del(db, "DELETE FROM pending_field_edits WHERE id = ?");
        del.bind(1, id);
        del.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ArchivePendingFieldEdit failed id=%lld reason=%s err=%s",
                  static_cast<long long>(id), terminalReason.c_str(), ex.what());
        throw;
    }
}

std::vector<DeadPendingFieldEdit> LocalCacheManager::LoadDeadPendingFieldEdits() {
    try {
        std::vector<DeadPendingFieldEdit> results;
        SQLite::Statement query(
            db, "SELECT dead_id, original_id, issue_key, field_id, fields_payload_json, attempts, last_error, "
                "created_at, archived_at, terminal_reason, COALESCE(backend_key, '') "
                "FROM pending_field_edits_dead ORDER BY archived_at DESC, dead_id DESC");
        while (query.executeStep()) {
            DeadPendingFieldEdit row;
            row.DeadId = query.getColumn(0).getInt64();
            row.OriginalId = query.getColumn(1).getInt64();
            row.IssueKey = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.FieldId = query.getColumn(3).isNull() ? std::string() : std::string(query.getColumn(3).getText());
            row.FieldsPayloadJson =
                query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.Attempts = query.getColumn(5).getInt();
            row.LastError = query.getColumn(6).isNull() ? std::string() : std::string(query.getColumn(6).getText());
            row.CreatedAtEpochSec = query.getColumn(7).getInt64();
            row.ArchivedAtEpochSec = query.getColumn(8).getInt64();
            row.TerminalReason =
                query.getColumn(9).isNull() ? std::string() : std::string(query.getColumn(9).getText());
            row.BackendKey = query.getColumn(10).isNull() ? std::string() : std::string(query.getColumn(10).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadDeadPendingFieldEdits failed err=%s", ex.what());
        throw;
    }
}

bool LocalCacheManager::RestoreDeadPendingFieldEdit(const std::int64_t originalPendingId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement sel(db, "SELECT dead_id, issue_key, field_id, fields_payload_json, original_rich_value, "
                                  "original_value, COALESCE(has_original_value, 0), COALESCE(backend_key, '') "
                                  "FROM pending_field_edits_dead WHERE original_id = ? "
                                  "ORDER BY dead_id DESC LIMIT 1");
        sel.bind(1, originalPendingId);
        if (!sel.executeStep()) {
            transaction.commit();
            return false;
        }
        const std::int64_t deadId = sel.getColumn(0).getInt64();
        const std::string issueKey =
            sel.getColumn(1).isNull() ? std::string() : std::string(sel.getColumn(1).getText());
        const std::string fieldId = sel.getColumn(2).isNull() ? std::string() : std::string(sel.getColumn(2).getText());
        const std::string payload = sel.getColumn(3).isNull() ? std::string() : std::string(sel.getColumn(3).getText());
        const bool richIsNull = sel.getColumn(4).isNull();
        const std::string originalRichValue = richIsNull ? std::string() : std::string(sel.getColumn(4).getText());
        const bool valueIsNull = sel.getColumn(5).isNull();
        const std::string originalValue = valueIsNull ? std::string() : std::string(sel.getColumn(5).getText());
        const bool hasOriginalValue = sel.getColumn(6).getInt() != 0;
        // Restore re-queues under the SAME backend the row was originally queued against
        // (multi-grid Slice 1c) — never the currently-focused context's key.
        const std::string backendKey =
            sel.getColumn(7).isNull() ? std::string() : std::string(sel.getColumn(7).getText());
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement ins(db, "INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, "
                                  "original_rich_value, original_value, has_original_value, backend_key, attempts, "
                                  "last_error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0, '', ?)");
        ins.bind(1, issueKey);
        ins.bind(2, fieldId);
        ins.bind(3, payload);
        if (richIsNull)
            ins.bind(4); // NULL
        else
            ins.bind(4, originalRichValue);
        if (valueIsNull)
            ins.bind(5); // NULL
        else
            ins.bind(5, originalValue);
        ins.bind(6, hasOriginalValue ? 1 : 0);
        ins.bind(7, backendKey);
        ins.bind(8, epoch);
        ins.exec();
        SQLite::Statement delDead(db, "DELETE FROM pending_field_edits_dead WHERE dead_id = ?");
        delDead.bind(1, deadId);
        delDead.exec();
        transaction.commit();
        LOG_INFO("LocalCacheManager::RestoreDeadPendingFieldEdit original_id=%lld dead_id=%lld",
                 static_cast<long long>(originalPendingId), static_cast<long long>(deadId));
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RestoreDeadPendingFieldEdit failed original_id=%lld err=%s",
                  static_cast<long long>(originalPendingId), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteDeadPendingFieldEdit(const std::int64_t deadId) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_field_edits_dead WHERE dead_id = ?");
        del.bind(1, deadId);
        del.exec();
        if (db.getChanges() == 0) {
            LOG_WARN("LocalCacheManager::DeleteDeadPendingFieldEdit: no row for dead_id=%lld",
                     static_cast<long long>(deadId));
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteDeadPendingFieldEdit failed dead_id=%lld err=%s",
                  static_cast<long long>(deadId), ex.what());
        throw;
    }
}

#if defined(SMATCHET_WITH_AI)

std::int64_t LocalCacheManager::AppendChatMessage(const AiMessage& msg) {
    try {
        SQLite::Statement ins(db, "INSERT INTO ai_chat_messages (created_at, role, content, pinned) "
                                  "VALUES (?, ?, ?, ?)");
        ins.bind(1, static_cast<std::int64_t>(msg.CreatedAtUnixMs));
        ins.bind(2, msg.Role);
        ins.bind(3, msg.Content);
        ins.bind(4, msg.Pinned ? 1 : 0);
        ins.exec();
        return db.getLastInsertRowid();
    } catch (const std::exception& ex) {
        // Pillar 3 graceful degradation — the UI keeps the in-memory copy; persistence
        // failures do not surface a modal. Same shape as ConfigManager::Save errors.
        LOG_WARN("LocalCacheManager::AppendChatMessage failed err=%s", ex.what());
        return -1;
    }
}

void LocalCacheManager::UpdateChatMessagePin(std::int64_t id, bool pinned) {
    try {
        SQLite::Statement upd(db, "UPDATE ai_chat_messages SET pinned = ? WHERE id = ?");
        upd.bind(1, pinned ? 1 : 0);
        upd.bind(2, static_cast<std::int64_t>(id));
        upd.exec();
        if (db.getChanges() == 0) {
            LOG_WARN("LocalCacheManager::UpdateChatMessagePin: no row id=%lld", static_cast<long long>(id));
        }
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::UpdateChatMessagePin failed id=%lld err=%s", static_cast<long long>(id),
                 ex.what());
    }
}

void LocalCacheManager::LoadChatMessages(std::size_t cap, std::vector<AiMessage>& outMessages,
                                         std::vector<std::int64_t>& outIds) {
    outMessages.clear();
    outIds.clear();
    if (cap == 0) {
        return;
    }
    try {
        // Subquery selects the most-recent `cap` by id-desc; outer query re-sorts
        // ascending so the UI displays oldest→newest in chronological order.
        SQLite::Statement q(db, "SELECT id, created_at, role, content, pinned FROM ("
                                "  SELECT id, created_at, role, content, pinned FROM ai_chat_messages "
                                "  ORDER BY id DESC LIMIT ?) "
                                "ORDER BY id ASC");
        q.bind(1, static_cast<std::int64_t>(cap));
        while (q.executeStep()) {
            AiMessage m;
            const std::int64_t id = q.getColumn(0).getInt64();
            m.CreatedAtUnixMs = q.getColumn(1).getInt64();
            m.Role = q.getColumn(2).isNull() ? std::string() : std::string(q.getColumn(2).getText());
            m.Content = q.getColumn(3).isNull() ? std::string() : std::string(q.getColumn(3).getText());
            m.Pinned = q.getColumn(4).getInt() != 0;
            outMessages.push_back(std::move(m));
            outIds.push_back(id);
        }
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::LoadChatMessages failed err=%s", ex.what());
        outMessages.clear();
        outIds.clear();
    }
}

void LocalCacheManager::ClearChatMessages() {
    try {
        db.exec("DELETE FROM ai_chat_messages");
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::ClearChatMessages failed err=%s", ex.what());
    }
}

void LocalCacheManager::TrimChatMessages(std::size_t maxRows) {
    try {
        // Pinned rows always survive trim; only non-pinned rows beyond the most-recent
        // `maxRows` are deleted. When `maxRows == 0` the inner subquery still matches no
        // rows, and the outer DELETE removes every non-pinned row — which is the
        // intended degenerate behaviour for cap=0.
        SQLite::Statement del(db, "DELETE FROM ai_chat_messages WHERE pinned = 0 AND id NOT IN ("
                                  "  SELECT id FROM ai_chat_messages WHERE pinned = 0 "
                                  "  ORDER BY id DESC LIMIT ?)");
        del.bind(1, static_cast<std::int64_t>(maxRows));
        del.exec();
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::TrimChatMessages failed maxRows=%zu err=%s", maxRows, ex.what());
    }
}

#endif // SMATCHET_WITH_AI
