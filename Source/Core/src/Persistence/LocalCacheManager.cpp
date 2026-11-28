#include "LocalCacheManager.h"
#include "LocalCacheManagerInternal.h"

#include "IssueDraft.h"
#include "Logger.h"
#include "Persistence/SqliteOpenRecoveryPure.h"
#include "Sync/RemoteVersionPure.h" // IsIncomingRowStale: SaveTicketsWithSyncMeta never rolls a row back
#include "TimeNowPure.h"
#include "UiThreadAffinity.h"

#include <ghc/filesystem.hpp>
#include <sqlite3.h> // SQLITE_NOTADB / SQLITE_CORRUPT result codes (corrupt-file classification)

#include <algorithm>
#include <atomic>
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

using lcm_detail::AddColumnIfMissing;

// A UI-thread wait for the connection longer than this is logged, at most once per interval.
constexpr long long kUiWaitWarnUs = 2000;
constexpr long long kUiWarnIntervalMs = 10000;

// Creates / migrates the offline field-edit queue tables: `pending_field_edits` plus its
// dead-letter twin. Extracted from the constructor so the schema-init body stays under the
// function-size cap. All migrations are additive — CREATE IF NOT EXISTS plus guarded ADD COLUMN —
// per the Persistence forward-only invariant.
void InitFieldEditQueueSchema(const LocalCacheManager::ConnGuard& guard, SQLite::Database& db) {
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
    AddColumnIfMissing(guard, db, "pending_field_edits", "original_rich_value", "TEXT");
    // ADR-0016: additive scalar conflict base (display value). Twin of original_rich_value.
    AddColumnIfMissing(guard, db, "pending_field_edits", "original_value", "TEXT");
    // ADR-0016: presence flag for the scalar base — distinguishes a genuinely BLANK captured base
    // ("" but present) from a legacy/no-base row, so a blank-field edit is still conflict-checked
    // instead of silently last-write-wins. Legacy rows default 0 (no base → last-write-wins).
    AddColumnIfMissing(guard, db, "pending_field_edits", "has_original_value", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(guard, db, "pending_field_edits", "has_merge_conflict", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(guard, db, "pending_field_edits", "conflict_context_json", "TEXT");
    // Multi-grid Slice 1c (ADR-0018 decision 4): additive backend namespace for queue rows.
    // Legacy rows land as '' and are backfilled once by RunOneTimePendingQueueBackendKeyStamp.
    AddColumnIfMissing(guard, db, "pending_field_edits", "backend_key", "TEXT NOT NULL DEFAULT ''");
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
    AddColumnIfMissing(guard, db, "pending_field_edits_dead", "original_rich_value", "TEXT");
    // ADR-0016: additive scalar conflict base twin on the dead-letter table.
    AddColumnIfMissing(guard, db, "pending_field_edits_dead", "original_value", "TEXT");
    // ADR-0016: presence-flag twin on the dead-letter table (mirrors pending_field_edits).
    AddColumnIfMissing(guard, db, "pending_field_edits_dead", "has_original_value", "INTEGER NOT NULL DEFAULT 0");
    AddColumnIfMissing(guard, db, "pending_field_edits_dead", "archived_at", "INTEGER NOT NULL DEFAULT 0");
    // Multi-grid Slice 1c: backend-namespace twin on the dead-letter table (carried over on
    // archive so the UI can attribute the row and a restore re-queues under the same backend).
    AddColumnIfMissing(guard, db, "pending_field_edits_dead", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_field_edits_dead_archived_at "
            "ON pending_field_edits_dead(archived_at DESC)");
}

// Creates the backend-key-namespaced ticket table family (multi-grid Slice 1b, ADR-0018
// decision 4). The PK change (id → (backend_key, id)) cannot be done additively in place, so
// the v2 tables are a versioned step; the legacy `tickets` / `ticket_field_values` /
// `ticket_field_rich_values` tables are retained on disk unused (Persistence additive-only
// invariant — no DROP/RENAME). The PK prefix covers all per-backend lookups — no extra indices.
void InitTicketsV2Schema(const LocalCacheManager::ConnGuard& /*guard*/, SQLite::Database& db) {
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
    // Delta-sync version metadata (docs/plans/active/delta-sync.md § Phases (phase 2)). A SEPARATE table, not a
    // column on tickets_v2: ticket saves are INSERT OR REPLACE, which would reset an added column on every
    // save. A missing row means "unknown version" - the ticket is simply refetched - so no migration flag.
    // `fields` is the newline-joined sorted list of field ids the row was fetched with.
    db.exec("CREATE TABLE IF NOT EXISTS ticket_sync_meta ("
            "backend_key TEXT NOT NULL, "
            "ticket_id TEXT NOT NULL, "
            "remote_updated TEXT NOT NULL, "
            "fields TEXT NOT NULL, "
            "synced_at INTEGER NOT NULL, "
            "PRIMARY KEY(backend_key, ticket_id))");
}

// `ticket_sync_meta.fields` stores the sorted field ids newline-joined (ids never contain a newline).
std::string JoinFieldList(const std::vector<std::string>& fields) {
    std::string joined;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            joined += '\n';
        }
        joined += fields[i];
    }
    return joined;
}

std::vector<std::string> SplitFieldList(const std::string& text) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        if (stop > start) {
            fields.push_back(text.substr(start, stop - start));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

} // namespace

LocalCacheManager::ConnGuard::ConnGuard(const LocalCacheManager& owner, const char* op) : owner_(owner) {
    const std::thread::id self = std::this_thread::get_id();
    // Only this thread can have stored its own id, so a relaxed load is exact: seeing `self` means a method on this
    // thread called back into a public method while holding the connection.
    if (owner.connOwner_.load(std::memory_order_relaxed) == self) {
        LOG_ERROR("LocalCacheManager::%s re-entered the connection on the thread that holds it", op);
        throw std::logic_error(std::string("LocalCacheManager::") + op + ": re-entered the connection guard");
    }
    if (UiThreadAffinity::IsOnUiThread()) {
        // A frame waits for whoever holds the connection: time it so a long worker operation is visible.
        const std::chrono::steady_clock::time_point waitStart = std::chrono::steady_clock::now();
        lock_ = std::unique_lock<std::mutex>(owner.connMutex_);
        const long long waitedUs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart)
                .count());
        if (waitedUs > kUiWaitWarnUs) {
            // One warning per 10 s: a busy worker would otherwise log on every frame.
            static std::atomic<long long> s_lastWarnMs{-kUiWarnIntervalMs};
            const long long nowMs = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               std::chrono::steady_clock::now().time_since_epoch())
                                                               .count());
            long long last = s_lastWarnMs.load(std::memory_order_relaxed);
            if (nowMs - last >= kUiWarnIntervalMs && s_lastWarnMs.compare_exchange_strong(last, nowMs)) {
                LOG_WARN("LocalCacheManager::%s: the UI thread waited %lld us for the cache connection", op, waitedUs);
            }
        }
    } else {
        lock_ = std::unique_lock<std::mutex>(owner.connMutex_);
    }
    owner.connOwner_.store(self, std::memory_order_relaxed);
}

LocalCacheManager::ConnGuard::~ConnGuard() {
    // Clear the owner BEFORE `lock_` releases (member destructors run after this body), so the next holder never
    // sees a stale id.
    owner_.connOwner_.store(std::thread::id(), std::memory_order_relaxed);
}

SQLite::Database& LocalCacheManager::conn(const ConnGuard& guard) {
    if (&guard.owner_ != this) {
        throw std::logic_error("LocalCacheManager::conn: the guard belongs to another manager");
    }
    return db_;
}

// The interruptible open's busy handler: sleeps 10 ms between lock attempts, and gives up (returns 0) once the abort
// flag is set or the usual 5 s busy timeout has elapsed. SQLite calls it from inside the statement that is waiting.
namespace lcm_detail {
struct OpenBusyState {
    const std::atomic<bool>* Abort = nullptr;
    std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
    bool AbortRequested() const { return Abort != nullptr && Abort->load(std::memory_order_acquire); }
};
} // namespace lcm_detail

namespace {

constexpr long long kOpenBusyTimeoutMs = 5000;

int OpenBusyHandler(void* context, int /*attempts*/) {
    const lcm_detail::OpenBusyState* state = static_cast<const lcm_detail::OpenBusyState*>(context);
    const long long waitedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - state->Start).count();
    if (state->AbortRequested() || waitedMs >= kOpenBusyTimeoutMs) {
        return 0;
    }
    sqlite3_sleep(10);
    return 1;
}

} // namespace

LocalCacheManager::LocalCacheManager(const std::string& dbPath) : LocalCacheManager(dbPath, OpenOptions()) {}

namespace {

/// The cache needs `INSERT ... RETURNING` (SQLite 3.35). The compile-time check covers the SQLite we build against;
/// this covers the library the process actually loaded, so an older one fails the open (a session-only cache) instead
/// of failing the first enqueue.
const std::string& CheckSqliteVersionForOpen(const std::string& dbPath) {
    // The memory-only session cache is the fallback for exactly this failure, and its readers cope without RETURNING.
    if (dbPath != ":memory:" && sqlite3_libversion_number() < lcm_detail::kMinSqliteVersionNumber) {
        throw std::runtime_error(std::string("SQLite ") + sqlite3_libversion() +
                                 " is too old for the local cache (3.35 or newer is required)");
    }
    return dbPath;
}

} // namespace

LocalCacheManager::LocalCacheManager(const std::string& dbPath, const OpenOptions& options)
    : dbPath_(CheckSqliteVersionForOpen(dbPath)),
      db_(dbPath_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX) {
    LOG_INFO("LocalCacheManager: opening db path=%s", dbPath_.c_str());
    lcm_detail::OpenBusyState busyState;
    busyState.Abort = options.Abort;
    if (options.Abort != nullptr) {
        openBusy_ = &busyState;
    }
    // Whatever way the constructor ends, the interruptible handler goes and the ordinary busy timeout returns.
    struct RestoreBusyTimeout {
        LocalCacheManager& Self;
        ~RestoreBusyTimeout() {
            Self.openBusy_ = nullptr;
            try {
                const ConnGuard guard(Self, "OpenRestore");
                Self.conn(guard).setBusyTimeout(static_cast<int>(kOpenBusyTimeoutMs));
            } catch (const std::exception& ex) {
                LOG_WARN("LocalCacheManager: could not restore the busy timeout: %s", ex.what());
            }
        }
    } restoreBusyTimeout{*this};
    {
        const ConnGuard guard(*this, "Open");
        ApplyWalPragmas(guard);
    }
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
    // succeeds, so they never reach this catch. The guard is taken per attempt and is released before the backoff
    // sleep (it is out of scope in the catch).
    constexpr int kMaxInitAttempts = 5;
    for (int attempt = 0;; ++attempt) {
        if (busyState.AbortRequested()) {
            throw std::runtime_error("LocalCacheManager: the open was aborted");
        }
        try {
            const ConnGuard guard(*this, "InitSchema");
            InitSchema(guard);
            break;
        } catch (const SQLite::Exception& ex) {
            const int code = ex.getErrorCode();
            if (smatchet::IsRebuildableCorruptCode(code) && options.RebuildCorruptFile) {
                const ConnGuard guard(*this, "RebuildFreshAfterCorruption");
                RebuildFreshAfterCorruption(guard, ex);
                break;
            }
            if (smatchet::ShouldRetryBusyInit(code, attempt, kMaxInitAttempts)) {
                const int backoffMs = smatchet::BusyRetryBackoffMs(attempt);
                if (busyState.AbortRequested()) {
                    throw std::runtime_error("LocalCacheManager: the open was aborted");
                }
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

void LocalCacheManager::RebuildFreshAfterCorruption(const ConnGuard& guard, const SQLite::Exception& ex) {
    SQLite::Database& db = conn(guard);
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
    ApplyWalPragmas(guard);
    InitSchema(guard);
}

void LocalCacheManager::ApplyWalPragmas(const ConnGuard& guard) {
    SQLite::Database& db = conn(guard);
    // Arm the busy-timeout FIRST (Slice-G Phase 3 / G2): the journal_mode=WAL pragma needs a
    // brief exclusive lock and can itself return SQLITE_BUSY under a concurrent open, so it must
    // run with the lock-wait already in effect; arming it first also guarantees the timeout is
    // set even if a later pragma throws. WAL improves crash safety and lets readers run while a
    // writer is active; synchronous=NORMAL is the recommended WAL pairing (fsync on checkpoint,
    // not every commit).
    try {
        db.setBusyTimeout(static_cast<int>(kOpenBusyTimeoutMs));
        if (openBusy_ != nullptr) {
            // Replaces the plain timeout above for the duration of the open, so an abort is seen within 10 ms.
            sqlite3_busy_handler(db.getHandle(), &OpenBusyHandler, openBusy_);
        }
        // The cache has no trigger, view or generated column, so nothing in its schema needs to run SQL functions: a
        // file carrying such objects (an adopted older file) must not get to run them.
        db.exec("PRAGMA trusted_schema=OFF");
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA synchronous=NORMAL");
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager: failed to set WAL pragmas: %s", ex.what());
    }
}

void LocalCacheManager::InitSchema(const ConnGuard& guard) {
    SQLite::Database& db = conn(guard);
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
    InitTicketsV2Schema(guard, db);
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "payload TEXT NOT NULL, "
            "backend_key TEXT NOT NULL DEFAULT '', "
            "attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL)");
    // Multi-grid Slice 1c (ADR-0018 decision 4): additive backend namespace for queue rows.
    // Legacy rows land as '' and are backfilled once by RunOneTimePendingQueueBackendKeyStamp.
    AddColumnIfMissing(guard, db, "pending_creates", "backend_key", "TEXT NOT NULL DEFAULT ''");
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
    AddColumnIfMissing(guard, db, "pending_creates_dead", "archived_at", "INTEGER NOT NULL DEFAULT 0");
    // Multi-grid Slice 1c: backend-namespace twin on the dead-letter table.
    AddColumnIfMissing(guard, db, "pending_creates_dead", "backend_key", "TEXT NOT NULL DEFAULT ''");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_creates_dead_archived_at "
            "ON pending_creates_dead(archived_at DESC)");
    db.exec("CREATE TABLE IF NOT EXISTS cache_meta ("
            "key TEXT PRIMARY KEY, "
            "value TEXT NOT NULL)");
    InitFieldEditQueueSchema(guard, db);
    InitLookupCacheSchema_(guard);
    InitLeftViewHistorySchema_(guard);
    InitPendingActionsSchema_(guard);
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

SQLite::Statement& LocalCacheManager::stmt(const ConnGuard& guard, std::unique_ptr<SQLite::Statement>& slot,
                                           const char* sql) {
    if (!slot) {
        // Freshly prepared statements have no bindings and no execution state to reset.
        slot = std::make_unique<SQLite::Statement>(conn(guard), sql);
        return *slot;
    }
    slot->reset();
    slot->clearBindings();
    return *slot;
}

// Write one ticket's rows using the cached prepared statements. Runs under the connection guard
// AND an enclosing SQLite::Transaction is active (SaveTicket / SaveTickets own the txn). Each
// single-bind statement is reset()+clearBindings() after exec so it can be rebound for the next
// ticket when called in a batch loop (SaveTickets).
void LocalCacheManager::writeTicketRows_(const ConnGuard& guard, const std::string& backendKey,
                                         const CachedTicket& ticket) {
    auto& ticketUpsert =
        stmt(guard, stmt_save_upsert_ticket_, "INSERT OR REPLACE INTO tickets_v2 (backend_key, id) VALUES (?, ?)");
    ticketUpsert.bind(1, backendKey);
    ticketUpsert.bind(2, ticket.id);
    ticketUpsert.exec();
    ticketUpsert.reset();
    ticketUpsert.clearBindings();

    // Keep selected field cache rows in sync with latest snapshot for this ticket.
    auto& deleteFields = stmt(guard, stmt_save_delete_fields_,
                              "DELETE FROM ticket_field_values_v2 WHERE backend_key = ? AND ticket_id = ?");
    deleteFields.bind(1, backendKey);
    deleteFields.bind(2, ticket.id);
    deleteFields.exec();
    deleteFields.reset();
    deleteFields.clearBindings();

    auto& fieldUpsert = stmt(guard, stmt_save_insert_field_,
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
    auto& deleteRich = stmt(guard, stmt_save_delete_rich_,
                            "DELETE FROM ticket_field_rich_values_v2 WHERE backend_key = ? AND ticket_id = ?");
    deleteRich.bind(1, backendKey);
    deleteRich.bind(2, ticket.id);
    deleteRich.exec();
    deleteRich.reset();
    deleteRich.clearBindings();

    auto& richUpsert = stmt(guard, stmt_save_insert_rich_,
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
    // The connection guard is taken per attempt inside RunTxn_, so it is not held across a backoff sleep.
    // A plain save rewrites the row without knowing which tracker version it holds, so the stored version no
    // longer vouches for it: drop the meta (the next delta sync refetches the ticket). SaveTicketsWithSyncMeta
    // is the writer that keeps it.
    RunTxn_("SaveTicket", [&](const ConnGuard& guard, SQLite::Database&) {
        writeTicketRows_(guard, backendKey, ticket);
        deleteSyncMeta_(guard, backendKey, ticket.id);
    });
}

// Phase 3(a) of docs/plans/shipped/memory-budget-and-lifetime-hardening.md: persist a whole slice
// of tickets in ONE transaction (the streaming-sync apply loop used to open a separate
// SQLite::Transaction per ticket — up to 20 commits/frame on the UI thread). Reuses the cached
// prepared statements across the batch.
void LocalCacheManager::SaveTickets(const std::string& backendKey, const std::vector<CachedTicket>& tickets) {
    if (tickets.empty()) {
        return;
    }
    RunTxn_("SaveTickets", [&](const ConnGuard& guard, SQLite::Database&) {
        for (const auto& ticket : tickets) {
            writeTicketRows_(guard, backendKey, ticket);
            deleteSyncMeta_(guard, backendKey, ticket.id); // see SaveTicket
        }
    });
}

bool LocalCacheManager::readTicketRows_(const ConnGuard& guard, const std::string& backendKey,
                                        const std::string& ticketId, CachedTicket& out) {
    out = CachedTicket{};
    out.id = ticketId;
    auto& exists = stmt(guard, stmt_get_exists_, "SELECT 1 FROM tickets_v2 WHERE backend_key = ? AND id = ? LIMIT 1");
    exists.bind(1, backendKey);
    exists.bind(2, ticketId);
    const bool present = exists.executeStep();
    exists.reset(); // never leave a read cursor open across the caller's writes
    if (!present) {
        return false;
    }
    auto& fieldQuery = stmt(guard, stmt_get_fields_,
                            "SELECT field_key, field_value FROM ticket_field_values_v2 "
                            "WHERE backend_key = ? AND ticket_id = ?");
    fieldQuery.bind(1, backendKey);
    fieldQuery.bind(2, ticketId);
    while (fieldQuery.executeStep()) {
        const std::string fieldKey = fieldQuery.getColumn(0).getText();
        out.fieldValues[fieldKey] =
            fieldQuery.getColumn(1).isNull() ? std::string() : std::string(fieldQuery.getColumn(1).getText());
    }
    auto& richQuery = stmt(guard, stmt_get_rich_,
                           "SELECT field_key, field_value FROM ticket_field_rich_values_v2 "
                           "WHERE backend_key = ? AND ticket_id = ?");
    richQuery.bind(1, backendKey);
    richQuery.bind(2, ticketId);
    while (richQuery.executeStep()) {
        const std::string fieldKey = richQuery.getColumn(0).getText();
        out.fieldRichValues[fieldKey] =
            richQuery.getColumn(1).isNull() ? std::string() : std::string(richQuery.getColumn(1).getText());
    }
    return true;
}

bool LocalCacheManager::TryGetTicket(const std::string& backendKey, const std::string& ticketId, CachedTicket& out) {
    try {
        const ConnGuard guard(*this, "TryGetTicket");
        const lcm_detail::ReadSnapshot snapshot(guard, conn(guard)); // the ticket row and its field rows agree
        return readTicketRows_(guard, backendKey, ticketId, out);
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::TryGetTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::deleteTicketRows_(const ConnGuard& guard, const std::string& backendKey,
                                          const std::string& ticketId) {
    // The four deletes of one ticket, on the cached statements (writeTicketRows_ and deleteSyncMeta_ prepare the
    // same SQL into the same slots); the caller owns the enclosing transaction and holds the connection guard.
    const std::pair<std::unique_ptr<SQLite::Statement>*, const char*> kDeletes[] = {
        {&stmt_save_delete_fields_, "DELETE FROM ticket_field_values_v2 WHERE backend_key = ? AND ticket_id = ?"},
        {&stmt_save_delete_rich_, "DELETE FROM ticket_field_rich_values_v2 WHERE backend_key = ? AND ticket_id = ?"},
        {&stmt_meta_delete_, "DELETE FROM ticket_sync_meta WHERE backend_key = ? AND ticket_id = ?"},
        {&stmt_delete_ticket_, "DELETE FROM tickets_v2 WHERE backend_key = ? AND id = ?"}};
    for (const auto& d : kDeletes) {
        SQLite::Statement& del = stmt(guard, *d.first, d.second);
        del.bind(1, backendKey);
        del.bind(2, ticketId);
        del.exec();
        del.reset();
        del.clearBindings();
    }
}

void LocalCacheManager::DeleteTicket(const std::string& backendKey, const std::string& ticketId) {
    try {
        RunTxn_("DeleteTicket",
                [&](const ConnGuard& guard, SQLite::Database&) { deleteTicketRows_(guard, backendKey, ticketId); });
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteTickets(const std::string& backendKey, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return;
    }
    try {
        RunTxn_("DeleteTickets", [&](const ConnGuard& guard, SQLite::Database&) {
            for (const std::string& id : ids) {
                deleteTicketRows_(guard, backendKey, id);
            }
        });
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteTickets failed rows=%zu err=%s", ids.size(), ex.what());
        throw;
    }
}

std::vector<CachedTicket> LocalCacheManager::GetTicketsByIds(const std::string& backendKey,
                                                             const std::vector<std::string>& ids) {
    std::vector<CachedTicket> rows;
    rows.reserve(ids.size());
    // Read in chunks and release the connection guard between them: the guard is shared with every UI-thread cache
    // call (the drain's saves, field edits), and a worker reading thousands of rows must not hold it throughout.
    constexpr std::size_t kChunk = 150;
    try {
        for (std::size_t offset = 0; offset < ids.size(); offset += kChunk) {
            const std::size_t end = (std::min)(ids.size(), offset + kChunk);
            const ConnGuard guard(*this, "GetTicketsByIds");
            const lcm_detail::ReadSnapshot snapshot(guard, conn(guard)); // one snapshot per chunk
            for (std::size_t i = offset; i < end; ++i) {
                CachedTicket row;
                if (readTicketRows_(guard, backendKey, ids[i], row)) {
                    rows.push_back(std::move(row));
                }
            }
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetTicketsByIds failed rows=%zu err=%s", ids.size(), ex.what());
        throw;
    }
    return rows;
}

bool LocalCacheManager::readSyncMeta_(const ConnGuard& guard, const std::string& backendKey,
                                      const std::string& ticketId, TicketSyncMeta& out) {
    auto& q = stmt(guard, stmt_meta_read_,
                   "SELECT remote_updated, fields, synced_at FROM ticket_sync_meta "
                   "WHERE backend_key = ? AND ticket_id = ?");
    q.bind(1, backendKey);
    q.bind(2, ticketId);
    const bool found = q.executeStep();
    if (found) {
        out.RemoteUpdated = q.getColumn(0).getText();
        out.Fields = SplitFieldList(q.getColumn(1).getText());
        out.SyncedAtEpochSec = q.getColumn(2).getInt64();
    }
    q.reset();
    return found;
}

void LocalCacheManager::writeSyncMeta_(const ConnGuard& guard, const std::string& backendKey,
                                       const std::string& ticketId, const TicketSyncMeta& meta) {
    auto& up = stmt(guard, stmt_meta_write_,
                    "INSERT OR REPLACE INTO ticket_sync_meta (backend_key, ticket_id, "
                    "remote_updated, fields, synced_at) VALUES (?, ?, ?, ?, ?)");
    up.bind(1, backendKey);
    up.bind(2, ticketId);
    up.bind(3, meta.RemoteUpdated);
    up.bind(4, JoinFieldList(meta.Fields));
    up.bind(5, static_cast<std::int64_t>(meta.SyncedAtEpochSec));
    up.exec();
    up.reset();
    up.clearBindings();
}

void LocalCacheManager::deleteSyncMeta_(const ConnGuard& guard, const std::string& backendKey,
                                        const std::string& ticketId) {
    auto& del = stmt(guard, stmt_meta_delete_, "DELETE FROM ticket_sync_meta WHERE backend_key = ? AND ticket_id = ?");
    del.bind(1, backendKey);
    del.bind(2, ticketId);
    del.exec();
    del.reset();
    del.clearBindings();
}

std::unordered_map<std::string, TicketSyncMeta> LocalCacheManager::GetTicketSyncMeta(const std::string& backendKey) {
    try {
        const ConnGuard guard(*this, "GetTicketSyncMeta");
        SQLite::Database& db = conn(guard);
        std::unordered_map<std::string, TicketSyncMeta> out;
        SQLite::Statement q(db, "SELECT ticket_id, remote_updated, fields, synced_at FROM ticket_sync_meta "
                                "WHERE backend_key = ?");
        q.bind(1, backendKey);
        while (q.executeStep()) {
            TicketSyncMeta meta;
            meta.RemoteUpdated = q.getColumn(1).getText();
            meta.Fields = SplitFieldList(q.getColumn(2).getText());
            meta.SyncedAtEpochSec = q.getColumn(3).getInt64();
            out[q.getColumn(0).getText()] = std::move(meta);
        }
        return out;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetTicketSyncMeta failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::SaveTicketsWithSyncMeta(const std::string& backendKey, const std::vector<CachedTicket>& tickets,
                                                const std::vector<TicketSyncMeta>& metas, bool overwriteOlder) {
    if (tickets.empty()) {
        return;
    }
    const std::int64_t nowMs = TimeNowPure::NowUnixMs();
    RunTxn_("SaveTicketsWithSyncMeta", [&](const ConnGuard& guard, SQLite::Database&) {
        for (std::size_t i = 0; i < tickets.size(); ++i) {
            const CachedTicket& ticket = tickets[i];
            TicketSyncMeta meta = i < metas.size() ? metas[i] : TicketSyncMeta();
            if (meta.RemoteUpdated.empty()) {
                writeTicketRows_(guard, backendKey, ticket); // unknown version: no meta, and drop a stale one
                deleteSyncMeta_(guard, backendKey, ticket.id);
                continue;
            }
            TicketSyncMeta existingMeta;
            CachedTicket existingRow;
            const bool hadMeta = readSyncMeta_(guard, backendKey, ticket.id, existingMeta);
            if (hadMeta && !overwriteOlder &&
                smatchet::sync::IsIncomingRowStale(meta.RemoteUpdated, existingMeta.RemoteUpdated, nowMs)) {
                // Provably older than the stored row (a late or reordered save): never roll it back.
                LOG_DEBUG("LocalCacheManager::SaveTicketsWithSyncMeta skipped stale row id=%s incoming=%s stored=%s",
                          ticket.id.c_str(), meta.RemoteUpdated.c_str(), existingMeta.RemoteUpdated.c_str());
                continue;
            }
            if (hadMeta && smatchet::sync::IsSameVersion(existingMeta, meta) &&
                readTicketRows_(guard, backendKey, ticket.id, existingRow)) {
                // Same tracker version fetched with other columns: keep both sets of values.
                writeTicketRows_(guard, backendKey, smatchet::sync::MergeSameVersionRow(existingRow, ticket));
                meta.Fields = smatchet::sync::UnionFields(existingMeta.Fields, meta.Fields);
            } else {
                writeTicketRows_(guard, backendKey, ticket);
            }
            writeSyncMeta_(guard, backendKey, ticket.id, meta);
        }
    });
}

void LocalCacheManager::SaveTicketKeepingSyncMeta(const std::string& backendKey, const CachedTicket& ticket) {
    RunTxn_("SaveTicketKeepingSyncMeta",
            [&](const ConnGuard& guard, SQLite::Database&) { writeTicketRows_(guard, backendKey, ticket); });
}

std::vector<CachedTicket> LocalCacheManager::GetAllTickets(const std::string& backendKey) {
    try {
        const ConnGuard guard(*this, "GetAllTickets");
        SQLite::Database& db = conn(guard);
        const lcm_detail::ReadSnapshot snapshot(guard, db); // the id list and both field queries agree
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

std::vector<std::string> LocalCacheManager::GetAllTicketIds(const std::string& backendKey) {
    try {
        const ConnGuard guard(*this, "GetAllTicketIds");
        SQLite::Database& db = conn(guard);
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
        // Per-table change counts (CR-948-5): `SQLite::Statement::exec()` returns the rows the statement itself
        // modified, so a partial re-run (e.g. ticket rows already copied by a crashed earlier pass, field/rich
        // rows not) still logs what THIS pass copied per table instead of a misleading 0 from a single trailing
        // `getChanges()`. The body returns the counts; the log waits for the commit.
        struct Copied {
            int tickets = 0;
            int fields = 0;
            int rich = 0;
        };
        const Copied copied =
            RunTxn_("RunOneTimeTicketsV2CopyMigration", [&](const ConnGuard& guard, SQLite::Database& db) -> Copied {
                Copied counts;
                if (hasCacheMetaFlag_(guard, kKey)) {
                    return counts;
                }
                // INSERT OR IGNORE keeps the copy idempotent against a partially-written v2 state
                // (e.g. a crash between copy and flag-set on a previous run).
                SQLite::Statement copyTickets(db, "INSERT OR IGNORE INTO tickets_v2 (backend_key, id) "
                                                  "SELECT ?, id FROM tickets");
                copyTickets.bind(1, backendKey);
                counts.tickets = copyTickets.exec();
                SQLite::Statement copyFields(db, "INSERT OR IGNORE INTO ticket_field_values_v2 "
                                                 "(backend_key, ticket_id, field_key, field_value) "
                                                 "SELECT ?, ticket_id, field_key, field_value FROM "
                                                 "ticket_field_values");
                copyFields.bind(1, backendKey);
                counts.fields = copyFields.exec();
                SQLite::Statement copyRich(db, "INSERT OR IGNORE INTO ticket_field_rich_values_v2 "
                                               "(backend_key, ticket_id, field_key, field_value) "
                                               "SELECT ?, ticket_id, field_key, field_value FROM "
                                               "ticket_field_rich_values");
                copyRich.bind(1, backendKey);
                counts.rich = copyRich.exec();
                setCacheMetaFlag_(guard, kKey);
                return counts;
            });
        if (copied.tickets > 0 || copied.fields > 0 || copied.rich > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimeTicketsV2CopyMigration copied tickets=%d field_rows=%d "
                     "rich_rows=%d backend_key=%s",
                     copied.tickets, copied.fields, copied.rich, backendKey.c_str());
        }
        return copied.tickets > 0 ? static_cast<size_t>(copied.tickets) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimeTicketsV2CopyMigration failed backend_key=%s err=%s",
                  backendKey.c_str(), ex.what());
        throw;
    }
}

bool LocalCacheManager::hasCacheMetaFlag_(const ConnGuard& guard, const std::string& key) {
    SQLite::Statement probe(conn(guard), "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
    probe.bind(1, key);
    return probe.executeStep();
}

void LocalCacheManager::setCacheMetaFlag_(const ConnGuard& guard, const std::string& key) {
    SQLite::Statement ins(conn(guard), "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
    ins.bind(1, key);
    ins.exec();
}

bool LocalCacheManager::HasCacheMetaFlag(const std::string& key) {
    try {
        const ConnGuard guard(*this, "HasCacheMetaFlag");
        return hasCacheMetaFlag_(guard, key);
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::HasCacheMetaFlag failed key=%s err=%s", key.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::SetCacheMetaFlag(const std::string& key) {
    try {
        const ConnGuard guard(*this, "SetCacheMetaFlag");
        setCacheMetaFlag_(guard, key);
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::SetCacheMetaFlag failed key=%s err=%s", key.c_str(), ex.what());
        throw;
    }
}

size_t LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts() {
    try {
        constexpr const char* kKey = "legacy_drop_pending_ge_max_attempts_v1";
        const int deleted = RunTxn_("RunOneTimeLegacyDropPendingAtMaxAttempts",
                                    [&](const ConnGuard& guard, SQLite::Database& db) -> int {
                                        if (hasCacheMetaFlag_(guard, kKey)) {
                                            return 0;
                                        }
                                        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE attempts >= ?");
                                        del.bind(1, OfflineCreateQueue::kMaxReplayAttempts);
                                        const int removed = del.exec();
                                        setCacheMetaFlag_(guard, kKey);
                                        return removed;
                                    });
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
        const size_t stamped = RunTxn_(
            "RunOneTimePendingQueueBackendKeyStamp", [&](const ConnGuard& guard, SQLite::Database& db) -> size_t {
                if (hasCacheMetaFlag_(guard, kKey)) {
                    return 0;
                }
                // Legacy rows were necessarily queued against the then-only configured backend, so
                // every un-stamped row ('' default from the additive ADD COLUMN) takes its key.
                static const char* const kTables[] = {"pending_creates", "pending_creates_dead", "pending_field_edits",
                                                      "pending_field_edits_dead"};
                size_t rows = 0;
                for (const char* table : kTables) {
                    const std::string sql =
                        std::string("UPDATE ") + table + " SET backend_key = ? WHERE backend_key = ''";
                    SQLite::Statement upd(db, sql);
                    upd.bind(1, backendKey);
                    const int changed = upd.exec();
                    rows += changed > 0 ? static_cast<size_t>(changed) : 0u;
                }
                setCacheMetaFlag_(guard, kKey);
                return rows;
            });
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

#if defined(SMATCHET_WITH_AI)

std::int64_t LocalCacheManager::AppendChatMessage(const AiMessage& msg) {
    try {
        const ConnGuard guard(*this, "AppendChatMessage");
        SQLite::Database& db = conn(guard);
        SQLite::Statement ins(db, "INSERT INTO ai_chat_messages (created_at, role, content, pinned) "
                                  "VALUES (?, ?, ?, ?) RETURNING id");
        ins.bind(1, static_cast<std::int64_t>(msg.CreatedAtUnixMs));
        ins.bind(2, msg.Role);
        ins.bind(3, msg.Content);
        ins.bind(4, msg.Pinned ? 1 : 0);
        return lcm_detail::StepReturningId(ins);
    } catch (const std::exception& ex) {
        // Pillar 3 graceful degradation — the UI keeps the in-memory copy; persistence
        // failures do not surface a modal. Same shape as ConfigManager::Save errors.
        LOG_WARN("LocalCacheManager::AppendChatMessage failed err=%s", ex.what());
        return -1;
    }
}

void LocalCacheManager::UpdateChatMessagePin(std::int64_t id, bool pinned) {
    try {
        const ConnGuard guard(*this, "UpdateChatMessagePin");
        SQLite::Database& db = conn(guard);
        SQLite::Statement upd(db, "UPDATE ai_chat_messages SET pinned = ? WHERE id = ?");
        upd.bind(1, pinned ? 1 : 0);
        upd.bind(2, static_cast<std::int64_t>(id));
        if (upd.exec() == 0) {
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
        const ConnGuard guard(*this, "LoadChatMessages");
        SQLite::Database& db = conn(guard);
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
            // SMATCHET_DEVIATION(rule=duplication; reason=SQLite NULL-to-empty row-mapping idiom shared with the dead-letter loaders in LocalCacheManager_PendingQueues.cpp; owner=orchestrator; revisit=column-text-helper)
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
        const ConnGuard guard(*this, "ClearChatMessages");
        conn(guard).exec("DELETE FROM ai_chat_messages");
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::ClearChatMessages failed err=%s", ex.what());
    }
}

void LocalCacheManager::TrimChatMessages(std::size_t maxRows) {
    try {
        const ConnGuard guard(*this, "TrimChatMessages");
        SQLite::Database& db = conn(guard);
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
