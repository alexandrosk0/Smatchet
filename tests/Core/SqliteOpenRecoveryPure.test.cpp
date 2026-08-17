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

using smatchet::BusyRetryBackoffMs;
using smatchet::IsDuplicateColumnRace;
using smatchet::IsRebuildableCorruptCode;
using smatchet::IsTransientBusyCode;
using smatchet::MakeCorruptQuarantineSuffix;
using smatchet::ShouldRetryBusyInit;
using smatchet::ShouldRetryBusyWrite;

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

// --- Slice-G Phase 3 / G2: transient-busy classification + retry policy ---

TEST_CASE("IsTransientBusyCode: lock/contention faults are transient (retryable)") {
    CHECK(IsTransientBusyCode(SQLITE_BUSY));
    CHECK(IsTransientBusyCode(SQLITE_BUSY_SNAPSHOT));
    CHECK(IsTransientBusyCode(SQLITE_LOCKED));
    CHECK(IsTransientBusyCode(SQLITE_PROTOCOL));
}

TEST_CASE("IsTransientBusyCode: corruption, permanent faults, and success are NOT transient") {
    // Disjoint from the corruption axis — a transient must never be quarantined, a corruption
    // must never be retried in place.
    CHECK_FALSE(IsTransientBusyCode(SQLITE_NOTADB));
    CHECK_FALSE(IsTransientBusyCode(SQLITE_CORRUPT));
    // Permanent open faults: a retry would never clear a permission/ENOENT error.
    CHECK_FALSE(IsTransientBusyCode(SQLITE_CANTOPEN));
    CHECK_FALSE(IsTransientBusyCode(SQLITE_IOERR));
    CHECK_FALSE(IsTransientBusyCode(SQLITE_PERM));
    CHECK_FALSE(IsTransientBusyCode(SQLITE_READONLY));
    CHECK_FALSE(IsTransientBusyCode(SQLITE_OK));
    CHECK_FALSE(IsTransientBusyCode(0));
    CHECK_FALSE(IsTransientBusyCode(-1));
}

TEST_CASE("IsTransientBusyCode and IsRebuildableCorruptCode are mutually exclusive") {
    // No code may be BOTH retryable and rebuildable — the ctor branches on corruption first,
    // then transient; an overlap would make the order load-bearing in a surprising way.
    const int codes[] = {SQLITE_BUSY,    SQLITE_LOCKED,   SQLITE_PROTOCOL, SQLITE_NOTADB,
                         SQLITE_CORRUPT, SQLITE_CANTOPEN, SQLITE_IOERR,    SQLITE_OK};
    for (int c : codes) {
        // Extra parens: doctest cannot decompose a raw `&&` inside CHECK_FALSE (it static-asserts
        // "Expression Too Complex"), so hand it a single pre-evaluated bool.
        const bool bothTrue = IsTransientBusyCode(c) && IsRebuildableCorruptCode(c);
        CHECK_FALSE(bothTrue);
    }
}

TEST_CASE("ShouldRetryBusyInit: retries a transient code only while the attempt budget remains") {
    // 0-based attempt; budget of 5 means attempts 0..3 retry, attempt 4 (the last) does not.
    CHECK(ShouldRetryBusyInit(SQLITE_BUSY, 0, 5));
    CHECK(ShouldRetryBusyInit(SQLITE_BUSY, 3, 5));
    CHECK_FALSE(ShouldRetryBusyInit(SQLITE_BUSY, 4, 5)); // last attempt — give up, re-throw
    CHECK_FALSE(ShouldRetryBusyInit(SQLITE_BUSY, 5, 5));
}

TEST_CASE("ShouldRetryBusyInit: never retries a non-transient code regardless of budget") {
    CHECK_FALSE(ShouldRetryBusyInit(SQLITE_CANTOPEN, 0, 5)); // permanent fault
    CHECK_FALSE(ShouldRetryBusyInit(SQLITE_NOTADB, 0, 5));   // corruption is rebuilt, not retried
    CHECK_FALSE(ShouldRetryBusyInit(SQLITE_OK, 0, 5));
}

// --- Concurrent additive-migration race (guarded ADD COLUMN check-then-act) ---

TEST_CASE("IsDuplicateColumnRace: SQLITE_ERROR with the duplicate-column message is the benign race") {
    // The loser of a concurrent guarded ADD COLUMN sees exactly this — SQLITE_ERROR (no dedicated
    // code) carrying SQLite's "duplicate column name: <col>" text. Its post-state is what the
    // migration wanted, so it is absorbed, not propagated.
    CHECK(IsDuplicateColumnRace(SQLITE_ERROR, "duplicate column name: has_merge_conflict"));
    CHECK(IsDuplicateColumnRace(SQLITE_ERROR, "duplicate column name"));
}

TEST_CASE("IsDuplicateColumnRace: a different SQLITE_ERROR message is NOT the race (must propagate)") {
    // Any other SQLITE_ERROR (a genuine SQL/typo bug) must surface, not be silently swallowed.
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_ERROR, "no such table: pending_field_edits"));
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_ERROR, "near \"ALTERx\": syntax error"));
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_ERROR, ""));
}

TEST_CASE("IsDuplicateColumnRace: non-SQLITE_ERROR codes and a null message are NOT the race") {
    // A transient BUSY is handled by the retry loop, not here; corruption is rebuilt. Only a plain
    // SQLITE_ERROR carries the duplicate-column text — nothing else may match.
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_BUSY, "duplicate column name: x"));
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_CORRUPT, "duplicate column name: x"));
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_OK, "duplicate column name: x"));
    CHECK_FALSE(IsDuplicateColumnRace(SQLITE_ERROR, nullptr));
}

TEST_CASE("IsDuplicateColumnRace is disjoint from the transient-busy and corruption axes") {
    // The three classification axes must not overlap: a code is at most one of transient / corrupt /
    // duplicate-column-race, so the ctor's branch order is not load-bearing.
    const bool raceIsTransient =
        IsDuplicateColumnRace(SQLITE_ERROR, "duplicate column name") && IsTransientBusyCode(SQLITE_ERROR);
    const bool raceIsCorrupt =
        IsDuplicateColumnRace(SQLITE_ERROR, "duplicate column name") && IsRebuildableCorruptCode(SQLITE_ERROR);
    CHECK_FALSE(raceIsTransient);
    CHECK_FALSE(raceIsCorrupt);
}

TEST_CASE("BusyRetryBackoffMs: capped exponential with a sub-half-second total budget") {
    CHECK(BusyRetryBackoffMs(-1) == 25); // negative clamps to attempt 0
    CHECK(BusyRetryBackoffMs(0) == 25);
    CHECK(BusyRetryBackoffMs(1) == 50);
    CHECK(BusyRetryBackoffMs(2) == 100);
    CHECK(BusyRetryBackoffMs(3) == 200);
    CHECK(BusyRetryBackoffMs(4) == 400);  // cap reached
    CHECK(BusyRetryBackoffMs(99) == 400); // stays capped
    // The 4 backoffs across a 5-attempt budget stay well under the multi-second busy_timeout
    // wall — a human-imperceptible startup hiccup, not a hang.
    CHECK(BusyRetryBackoffMs(0) + BusyRetryBackoffMs(1) + BusyRetryBackoffMs(2) + BusyRetryBackoffMs(3) == 375);
}

// --- issue #2045: the write-transaction retry loop's TOTAL wall-clock deadline ---

TEST_CASE("ShouldRetryBusyWrite: inside the budget it behaves exactly like the attempt-count policy") {
    // With time to spare, the write policy must not be stricter than the init policy — the
    // deadline is an ADDITIONAL bound, not a replacement.
    CHECK(ShouldRetryBusyWrite(SQLITE_BUSY, 0, 5, /*elapsedMs=*/0, /*budgetMs=*/500));
    CHECK(ShouldRetryBusyWrite(SQLITE_BUSY, 3, 5, /*elapsedMs=*/100, /*budgetMs=*/500));
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_BUSY, 4, 5, /*elapsedMs=*/0, /*budgetMs=*/500)); // attempts exhausted
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_CANTOPEN, 0, 5, /*elapsedMs=*/0, /*budgetMs=*/500)); // not transient
}

TEST_CASE("ShouldRetryBusyWrite: a spent budget stops the loop even with attempts left") {
    // The regression this closes: five attempts each sitting on the 5 s busy_timeout blocked the
    // caller ~25 s because only the attempt count was checked. One slow attempt now ends it.
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_BUSY, 0, 5, /*elapsedMs=*/5000, /*budgetMs=*/500));
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_BUSY, 1, 5, /*elapsedMs=*/501, /*budgetMs=*/500));
}

TEST_CASE("ShouldRetryBusyWrite: the next backoff is charged against the deadline, not slept past it") {
    // attempt 0 backs off 25 ms: allowed at 475 ms elapsed (475 + 25 == 500), refused at 476.
    CHECK(ShouldRetryBusyWrite(SQLITE_BUSY, 0, 5, /*elapsedMs=*/475, /*budgetMs=*/500));
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_BUSY, 0, 5, /*elapsedMs=*/476, /*budgetMs=*/500));
    // attempt 3 backs off 200 ms, so it needs 200 ms of headroom rather than 25.
    CHECK(ShouldRetryBusyWrite(SQLITE_BUSY, 3, 5, /*elapsedMs=*/300, /*budgetMs=*/500));
    CHECK_FALSE(ShouldRetryBusyWrite(SQLITE_BUSY, 3, 5, /*elapsedMs=*/301, /*budgetMs=*/500));
}

TEST_CASE("ShouldRetryBusyWrite: the whole 5-attempt ladder fits a 500 ms budget when attempts fail fast") {
    // The common contention shape — SQLITE_BUSY_SNAPSHOT returns immediately — must still get its
    // full retry ladder: the 375 ms of backoffs alone never exhaust the budget.
    long long elapsedMs = 0;
    int retries = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!ShouldRetryBusyWrite(SQLITE_BUSY_SNAPSHOT, attempt, 5, elapsedMs, /*budgetMs=*/500)) {
            break;
        }
        elapsedMs += BusyRetryBackoffMs(attempt);
        ++retries;
    }
    CHECK(retries == 4); // 4 retries after the first failure = the full 5-attempt budget
    CHECK(elapsedMs == 375);
}
