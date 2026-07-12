// Pure-logic coverage of IsRebuildableCorruptCode / MakeCorruptQuarantineSuffix — the
// classification that decides whether a SQLite result code surfacing during LocalCacheManager
// schema-init is genuine on-disk corruption (drop + rebuild) or a transient fault that must be
// re-thrown so a healthy-but-locked cache is never nuked (Slice-G Phase 2, #1352). The
// file-backed corruption cases that a real temp DB can materialize (garbage / truncated header /
// zero-byte / healthy) are covered by LocalCacheManagerCorruption.test.cpp; these cases pin the
// transient codes that a unit test cannot reliably force from disk — the "don't quarantine on a
// transient" contract itself.

#include "Persistence/SqliteOpenRecoveryPure.h"

#include <doctest/doctest.h>

#include <sqlite3.h>

using smatchet::IsRebuildableCorruptCode;
using smatchet::MakeCorruptQuarantineSuffix;

TEST_CASE("IsRebuildableCorruptCode: genuine on-disk corruption is rebuildable") {
    CHECK(IsRebuildableCorruptCode(SQLITE_NOTADB));
    CHECK(IsRebuildableCorruptCode(SQLITE_CORRUPT));
}

TEST_CASE("IsRebuildableCorruptCode: transient / lock / IO faults are NOT rebuildable (must re-throw)") {
    // The core contract: a momentarily-locked or unreadable but HEALTHY cache must never be
    // quarantined. None of these codes may map to a rebuild.
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_BUSY));
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_LOCKED));
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_CANTOPEN));
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_IOERR));
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_PERM));
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_READONLY));
}

TEST_CASE("IsRebuildableCorruptCode: success and out-of-range codes are NOT rebuildable") {
    CHECK_FALSE(IsRebuildableCorruptCode(SQLITE_OK));
    CHECK_FALSE(IsRebuildableCorruptCode(0));
    CHECK_FALSE(IsRebuildableCorruptCode(-1));
    CHECK_FALSE(IsRebuildableCorruptCode(999999));
}

TEST_CASE("MakeCorruptQuarantineSuffix: fixed timestamp formats as .corrupt-<unixSeconds>") {
    CHECK(MakeCorruptQuarantineSuffix(0) == ".corrupt-0");
    CHECK(MakeCorruptQuarantineSuffix(1752300000LL) == ".corrupt-1752300000");
}
