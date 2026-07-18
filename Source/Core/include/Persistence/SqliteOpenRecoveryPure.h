#pragma once

// SqliteOpenRecoveryPure — the pure classification behind LocalCacheManager's corrupt-cache
// recovery: given the SQLite result code that surfaced while initializing the schema of an
// on-disk cache, decide whether it is genuine on-disk corruption a fresh rebuild can recover
// from, versus a transient fault that must be re-thrown so a momentarily-locked/unreadable
// (but healthy) cache is never nuked (Slice-G Phase 2, #1352). Pure + header-only so the
// "don't quarantine on a transient" contract is unit-testable without a real SQLite handle,
// a temp file, or a rebuild round-trip — the file-backed cases a real DB can materialize
// (garbage / truncated / zero-byte) live in LocalCacheManagerCorruption.test.cpp; the
// transient codes it cannot easily force (BUSY / CANTOPEN / IOERR) are pinned here.

#include <sqlite3.h> // SQLITE_NOTADB / SQLITE_CORRUPT result-code constants (no link needed)

#include <cstring>
#include <ctime>
#include <string>

namespace smatchet {

/// True only for genuine on-disk corruption that dropping the file and rebuilding recovers:
/// SQLITE_NOTADB (not a database — e.g. garbage or a truncated header) or SQLITE_CORRUPT.
/// Everything else — a transient SQLITE_BUSY (momentarily locked), SQLITE_CANTOPEN /
/// SQLITE_IOERR (permission / ENOENT race), SQLITE_LOCKED, or a clean SQLITE_OK — is NOT
/// rebuildable and must be re-thrown so a healthy cache is never quarantined.
inline bool IsRebuildableCorruptCode(int sqliteErrorCode) {
    return sqliteErrorCode == SQLITE_NOTADB || sqliteErrorCode == SQLITE_CORRUPT;
}

/// The quarantine-path suffix appended to the corrupt cache file (and its -wal/-shm sidecars)
/// before a fresh rebuild, so the bad file is preserved for post-mortem rather than deleted.
/// Pure in its input so the format is pinned without reading the wall clock.
inline std::string MakeCorruptQuarantineSuffix(long long unixSeconds) {
    return ".corrupt-" + std::to_string(unixSeconds);
}

// --- Slice-G Phase 3 / G2: transient-contention (SQLITE_BUSY) classification ---
//
// Separate from the corruption axis above. When a SECOND Smatchet process opens the same
// on-disk cache, schema-init can hit a transient write-lock conflict. busy_timeout already
// makes SQLite WAIT on lock acquisition, but some paths (a WAL snapshot conflict) return a
// busy code immediately, and a failed WAL pragma can leave the timeout un-armed — so
// LocalCacheManager wraps schema-init in a bounded backoff-and-retry keyed on these codes,
// instead of crashing the ctor (Pillar 3 — never crash). These are NOT corruption (never
// quarantine) and NOT a permanent fault like SQLITE_CANTOPEN (retry would never clear it).

/// True for a TRANSIENT lock/contention fault a brief backoff-and-retry can clear:
/// SQLITE_BUSY (another connection holds a conflicting lock), SQLITE_BUSY_SNAPSHOT (a WAL
/// snapshot conflict on the write path — extended code, included for callers that pass the
/// extended result), SQLITE_LOCKED (a table/shared-cache lock), or SQLITE_PROTOCOL (a
/// legacy locking-protocol retry hint). Distinct from IsRebuildableCorruptCode: a transient
/// code is neither rebuilt nor a permanent failure — it is retried.
inline bool IsTransientBusyCode(int sqliteErrorCode) {
    return sqliteErrorCode == SQLITE_BUSY || sqliteErrorCode == SQLITE_BUSY_SNAPSHOT ||
           sqliteErrorCode == SQLITE_LOCKED || sqliteErrorCode == SQLITE_PROTOCOL;
}

/// Decide whether schema-init should be retried after a fault: retry only while the code is
/// transient-busy AND attempts remain. `attempt` is 0-based (0 = the first try that just
/// failed); `maxAttempts` is the total budget. Pure so the retry policy is unit-testable
/// without forcing real lock contention (which a doctest can only flakily reproduce).
inline bool ShouldRetryBusyInit(int sqliteErrorCode, int attempt, int maxAttempts) {
    return IsTransientBusyCode(sqliteErrorCode) && (attempt + 1) < maxAttempts;
}

// --- Concurrent additive-migration race (guarded ADD COLUMN check-then-act) ---

/// True when a failed `ADD COLUMN` is the benign concurrent-migration race: two initializers open
/// the same on-disk cache, both read the column as absent past the `table_info` guard, and the
/// loser's ALTER fails with SQLite's "duplicate column name" text. It carries no dedicated result
/// code (a plain SQLITE_ERROR), so matching the message is unavoidable — pinned + unit-tested here
/// rather than inlined. Post-state is what the migration wanted, so callers absorb it instead of
/// retrying or propagating. A null message is "not the race" (propagate).
inline bool IsDuplicateColumnRace(int sqliteErrorCode, const char* message) {
    if (sqliteErrorCode != SQLITE_ERROR || message == nullptr) {
        return false;
    }
    return std::strstr(message, "duplicate column name") != nullptr;
}

/// Backoff (ms) before the next schema-init retry — a capped exponential from a small base
/// (25 → 50 → 100 → 200 → 400 ms, then flat) so the total wait across the default budget is a
/// few hundred ms (a human-imperceptible startup hiccup), not the multi-second busy_timeout
/// wall. `attempt` is 0-based; negatives clamp to 0.
inline int BusyRetryBackoffMs(int attempt) {
    if (attempt < 0) {
        attempt = 0;
    }
    int ms = 25;
    for (int i = 0; i < attempt && ms < 400; ++i) {
        ms *= 2;
    }
    return ms > 400 ? 400 : ms;
}

} // namespace smatchet
