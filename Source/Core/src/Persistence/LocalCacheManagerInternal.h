#pragma once

// LocalCacheManagerInternal — private to the LocalCacheManager*.cpp translation units: the one write-transaction
// runner (`RunTxn_`) and the schema helpers that take the connection witness. Not installed, not for callers.

#include "Logger.h"
#include "Persistence/LocalCacheManager.h"
#include "Persistence/SqliteOpenRecoveryPure.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread> // sleep_for: bounded backoff between transaction attempts
#include <type_traits>
#include <utility>

namespace lcm_detail {

/// Total wall-clock budget for retrying one write transaction on a transient BUSY/LOCKED (issue #2045). The attempt
/// count alone bounds nothing a user can feel: each attempt may sit for the whole armed busy_timeout (5 s) before
/// SQLite hands back BUSY.
constexpr long long kWriteRetryBudgetMs = 500;

/// Holds a transaction body's result across attempts. A void body has nothing to hold.
template <typename R> struct TxnSlot {
    R value = R();
    template <typename Fn> void Run(Fn&& fn) { value = fn(); }
    R Take() { return std::move(value); }
};
template <> struct TxnSlot<void> {
    template <typename Fn> void Run(Fn&& fn) { fn(); }
    void Take() {}
};

/// True if `table` has column `col` (identifier names only; must be fixed literals at call sites). The guard proves
/// the caller holds the connection.
inline bool SqliteTableHasColumn(const LocalCacheManager::ConnGuard& /*guard*/, const SQLite::Database& db,
                                 const char* table, const char* col) {
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
/// The `table_info` guard is the fast path: an already-migrated cache runs no ALTER and throws no exception. The
/// catch closes the check-then-act TOCTOU window: two initializers opening the same on-disk cache can both read the
/// column as absent, and the loser's ALTER then fails with "duplicate column name". That specific race is idempotent
/// (the column the loser wanted now exists), so it is swallowed; every other failure propagates untouched.
/// `col`/`columnDef` are fixed literals at all call sites (no user input reaches the SQL).
inline void AddColumnIfMissing(const LocalCacheManager::ConnGuard& guard, SQLite::Database& db, const char* table,
                               const char* col, const char* columnDef) {
    if (SqliteTableHasColumn(guard, db, table, col)) {
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

} // namespace lcm_detail

/// Runs `body(guard, db)` inside one transaction on the shared connection and returns its value (or nothing for a
/// void body), retrying the whole attempt on a transient BUSY/LOCKED with the same bounded backoff the constructor
/// uses for schema-init (Slice-G Phase 3 / G2): a SECOND process writing the shared file can surface a BUSY that
/// busy_timeout does NOT absorb (a WAL snapshot or write-lock conflict is returned immediately). On a transient code
/// the SQLite::Transaction destructor rolls the attempt back before the next one begins. Non-transient errors and an
/// exhausted budget propagate unchanged (never-crash under contention, Quality Pillar 3).
///
/// The connection guard is taken PER ATTEMPT around the whole BEGIN…COMMIT and released on unwind BEFORE the backoff
/// sleep, so a retrying writer never holds the lock while sleeping. Because a body can run more than once it must be
/// replay-safe: it returns its outputs (never writes through a captured reference) and does no non-SQLite side
/// effect; log and publish after the call returns.
template <typename Body>
auto LocalCacheManager::RunTxn_(const char* op, Body&& body) -> decltype(body(std::declval<const ConnGuard&>(),
                                                                              std::declval<SQLite::Database&>())) {
    using Result = decltype(body(std::declval<const ConnGuard&>(), std::declval<SQLite::Database&>()));
    constexpr int kMaxWriteAttempts = 5;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    lcm_detail::TxnSlot<Result> slot;
    for (int attempt = 0;; ++attempt) {
        try {
            const ConnGuard guard(*this, op); // released on unwind BEFORE the backoff sleep below
            SQLite::Database& db = conn(guard);
            SQLite::Transaction transaction(db);
            slot.Run([&]() -> Result { return body(guard, db); });
            transaction.commit();
            return slot.Take();
        } catch (const SQLite::Exception& ex) {
            const long long elapsedMs = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count());
            if (smatchet::ShouldRetryBusyWrite(ex.getErrorCode(), attempt, kMaxWriteAttempts, elapsedMs,
                                               lcm_detail::kWriteRetryBudgetMs)) {
                const int backoffMs = smatchet::BusyRetryBackoffMs(attempt);
                LOG_WARN("LocalCacheManager::%s contended (%s); retry %d/%d after %d ms (%lld/%lld ms of budget "
                         "spent)",
                         op, ex.what(), attempt + 2, kMaxWriteAttempts, backoffMs, elapsedMs,
                         lcm_detail::kWriteRetryBudgetMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }
            LOG_ERROR("LocalCacheManager::%s failed after %lld ms err=%s", op, elapsedMs, ex.what());
            throw;
        }
    }
}
