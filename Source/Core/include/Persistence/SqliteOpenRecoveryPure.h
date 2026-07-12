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

} // namespace smatchet
