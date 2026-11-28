#include "LocalCacheManager.h"
#include "LocalCacheManagerInternal.h"

#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// LocalCacheManager's pending-action queue (Quality Pillar 6): comments, worklogs and watches
// saved while the tracker is unreachable, replayed by PendingActionQueueService. Additive
// tables; an older cache file gains them on open. Every method holds the connection guard (connMutex_) for its
// whole operation and uses its own local SQLite::Statement.
// Failures are logged and rethrown — the service counts and retries them, like the field-edit queue.

namespace {

std::string ColumnText(const SQLite::Column& column) {
    return column.isNull() ? std::string() : std::string(column.getText());
}

std::int64_t NowEpochSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

void BindAll(SQLite::Statement& /*stmt*/, int /*index*/) {}

// Binds `value, rest...` to parameters index, index + 1, … of `stmt`.
template <typename T, typename... Rest>
void BindAll(SQLite::Statement& stmt, int index, const T& value, const Rest&... rest) {
    stmt.bind(index, value);
    BindAll(stmt, index + 1, rest...);
}

// One parameterised write; returns the number of rows it changed.
template <typename... Args>
int ExecBound(const LocalCacheManager::ConnGuard& /*guard*/, SQLite::Database& db, const char* sql,
              const Args&... args) {
    SQLite::Statement stmt(db, sql);
    BindAll(stmt, 1, args...);
    return stmt.exec();
}

// The queue's error contract: log the failure with its row id, then rethrow for the caller to count.
template <typename Fn> auto LoggedQueueOp(const char* op, std::int64_t id, Fn&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::%s failed id=%lld err=%s", op, static_cast<long long>(id), ex.what());
        throw;
    }
}

// A pending_actions row starting at column `first`, in this column order. The dead table stores the
// same columns with `original_id` in place of `id`.
constexpr const char* kRowColumns =
    "id, backend_key, kind, issue_key, payload_json, state, attempts, last_error, created_at";
constexpr const char* kDeadRowColumns =
    "original_id, backend_key, kind, issue_key, payload_json, state, attempts, last_error, created_at";

PendingActionRecord ReadRow(SQLite::Statement& query, int first) {
    PendingActionRecord row;
    row.Id = query.getColumn(first).getInt64();
    row.BackendKey = ColumnText(query.getColumn(first + 1));
    row.Kind = ColumnText(query.getColumn(first + 2));
    row.IssueKey = ColumnText(query.getColumn(first + 3));
    row.PayloadJson = ColumnText(query.getColumn(first + 4));
    row.State = ColumnText(query.getColumn(first + 5));
    row.Attempts = query.getColumn(first + 6).getInt();
    row.LastError = ColumnText(query.getColumn(first + 7));
    row.CreatedAtEpochSec = query.getColumn(first + 8).getInt64();
    return row;
}

} // namespace

void LocalCacheManager::InitPendingActionsSchema_(const ConnGuard& guard) {
    SQLite::Database& db = conn(guard);
    db.exec("CREATE TABLE IF NOT EXISTS pending_actions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, backend_key TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL, "
            "issue_key TEXT NOT NULL, payload_json TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', "
            "attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_actions_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, "
            "backend_key TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL, issue_key TEXT NOT NULL, "
            "payload_json TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL, "
            "last_error TEXT, created_at INTEGER NOT NULL, archived_at INTEGER NOT NULL, "
            "terminal_reason TEXT NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_actions_backend ON pending_actions(backend_key, created_at)");
    // A row still `sending` was cut off mid-request by the previous run (crash, kill, power loss). The
    // tracker may or may not have applied it, so replay checks before sending it again.
    db.exec("UPDATE pending_actions SET state = 'ambiguous' WHERE state = 'sending'");
}

std::int64_t LocalCacheManager::EnqueuePendingAction(const std::string& backendKey, const std::string& kind,
                                                     const std::string& issueKey, const std::string& payloadJson,
                                                     const std::string& state) {
    return LoggedQueueOp("EnqueuePendingAction", 0, [&]() {
        const ConnGuard guard(*this, "EnqueuePendingAction");
        SQLite::Database& db = conn(guard);
        // RETURNING reads the id from this statement itself: last_insert_rowid() on the shared
        // connection could report another thread's concurrent insert.
        SQLite::Statement insert(db, "INSERT INTO pending_actions (backend_key, kind, issue_key, payload_json, "
                                     "state, attempts, last_error, created_at) VALUES (?, ?, ?, ?, ?, 0, '', ?) "
                                     "RETURNING id");
        BindAll(insert, 1, backendKey, kind, issueKey, payloadJson, state, NowEpochSec());
        if (!insert.executeStep()) {
            throw std::runtime_error("pending_actions insert returned no id");
        }
        return insert.getColumn(0).getInt64();
    });
}

std::vector<PendingActionRecord> LocalCacheManager::LoadPendingActions() {
    return LoggedQueueOp("LoadPendingActions", 0, [&]() {
        const ConnGuard guard(*this, "LoadPendingActions");
        SQLite::Database& db = conn(guard);
        std::vector<PendingActionRecord> rows;
        SQLite::Statement query(db, std::string("SELECT ") + kRowColumns + " FROM pending_actions ORDER BY id ASC");
        while (query.executeStep()) {
            rows.push_back(ReadRow(query, 0));
        }
        return rows;
    });
}

void LocalCacheManager::UpdatePendingAction(std::int64_t id, const std::string& state, int attempts,
                                            const std::string& lastError) {
    LoggedQueueOp("UpdatePendingAction", id, [&]() {
        const ConnGuard guard(*this, "UpdatePendingAction");
        ExecBound(guard, conn(guard), "UPDATE pending_actions SET state = ?, attempts = ?, last_error = ? WHERE id = ?",
                  state, attempts, lastError, id);
    });
}

bool LocalCacheManager::TransitionPendingAction(std::int64_t id, const std::string& fromState,
                                                const std::string& toState, int attempts,
                                                const std::string& lastError) {
    return LoggedQueueOp("TransitionPendingAction", id, [&]() {
        const ConnGuard guard(*this, "TransitionPendingAction");
        // One statement, so the state check and the write are atomic against a concurrent discard.
        return ExecBound(
                   guard, conn(guard),
                   "UPDATE pending_actions SET state = ?, attempts = ?, last_error = ? WHERE id = ? AND state = ?",
                   toState, attempts, lastError, id, fromState) == 1;
    });
}

void LocalCacheManager::DeletePendingAction(std::int64_t id) {
    LoggedQueueOp("DeletePendingAction", id, [&]() {
        const ConnGuard guard(*this, "DeletePendingAction");
        ExecBound(guard, conn(guard), "DELETE FROM pending_actions WHERE id = ?", id);
    });
}

void LocalCacheManager::ArchivePendingAction(std::int64_t id, const std::string& terminalReason,
                                             const std::string& terminalError) {
    LoggedQueueOp("ArchivePendingAction", id, [&]() {
        // A missing row throws runtime_error, which is not a transient code: the attempt rolls back and propagates.
        RunTxn_("ArchivePendingAction", [&](const ConnGuard& guard, SQLite::Database& db) {
            // One INSERT … SELECT copies the row as stored; only last_error may be replaced.
            const int copied =
                ExecBound(guard, db,
                          "INSERT INTO pending_actions_dead (original_id, backend_key, kind, issue_key, "
                          "payload_json, state, attempts, last_error, created_at, archived_at, "
                          "terminal_reason) SELECT id, backend_key, kind, issue_key, payload_json, state, "
                          "attempts, CASE WHEN ? <> '' THEN ? ELSE last_error END, created_at, ?, ? "
                          "FROM pending_actions WHERE id = ?",
                          terminalError, terminalError, NowEpochSec(), terminalReason, id);
            if (copied != 1) {
                throw std::runtime_error("pending_actions row not found");
            }
            ExecBound(guard, db, "DELETE FROM pending_actions WHERE id = ?", id);
        });
    });
}

std::vector<DeadPendingAction> LocalCacheManager::LoadDeadPendingActions() {
    return LoggedQueueOp("LoadDeadPendingActions", 0, [&]() {
        const ConnGuard guard(*this, "LoadDeadPendingActions");
        SQLite::Database& db = conn(guard);
        std::vector<DeadPendingAction> rows;
        SQLite::Statement query(db, std::string("SELECT dead_id, archived_at, terminal_reason, ") + kDeadRowColumns +
                                        " FROM pending_actions_dead ORDER BY archived_at DESC, dead_id DESC");
        while (query.executeStep()) {
            DeadPendingAction dead;
            dead.DeadId = query.getColumn(0).getInt64();
            dead.ArchivedAtEpochSec = query.getColumn(1).getInt64();
            dead.TerminalReason = ColumnText(query.getColumn(2));
            dead.Row = ReadRow(query, 3);
            rows.push_back(std::move(dead));
        }
        return rows;
    });
}

bool LocalCacheManager::RestoreDeadPendingAction(std::int64_t originalId) {
    return LoggedQueueOp("RestoreDeadPendingAction", originalId, [&]() {
        return RunTxn_("RestoreDeadPendingAction", [&](const ConnGuard& guard, SQLite::Database& db) -> bool {
            SQLite::Statement newest(db, "SELECT dead_id FROM pending_actions_dead WHERE original_id = ? "
                                         "ORDER BY dead_id DESC LIMIT 1");
            newest.bind(1, originalId);
            if (!newest.executeStep()) {
                return false;
            }
            const std::int64_t deadId = newest.getColumn(0).getInt64();
            // Same backend key and state as when it was archived (an ambiguous comment is still checked
            // before it is resent); attempts start over.
            ExecBound(guard, db,
                      "INSERT INTO pending_actions (backend_key, kind, issue_key, payload_json, state, attempts, "
                      "last_error, created_at) SELECT backend_key, kind, issue_key, payload_json, state, 0, '', ? "
                      "FROM pending_actions_dead WHERE dead_id = ?",
                      NowEpochSec(), deadId);
            ExecBound(guard, db, "DELETE FROM pending_actions_dead WHERE dead_id = ?", deadId);
            return true;
        });
    });
}

void LocalCacheManager::DeleteDeadPendingAction(std::int64_t deadId) {
    LoggedQueueOp("DeleteDeadPendingAction", deadId, [&]() {
        const ConnGuard guard(*this, "DeleteDeadPendingAction");
        ExecBound(guard, conn(guard), "DELETE FROM pending_actions_dead WHERE dead_id = ?", deadId);
    });
}
