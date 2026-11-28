// LocalCacheManager's pending-action queue, file-backed cases (Quality Pillar 6): a comment saved
// offline survives closing and reopening the cache (it is still sent after a restart), a row an
// interrupted run left `sending` becomes `ambiguous` on the next open (so replay checks the tracker
// before sending it again), and a cache file created before the tables existed gains them on open
// (additive-only schema).
//
// File-backed by necessity: each case needs a second connection to see the first one's bytes, and
// ":memory:" is per-connection (the in-memory cases are in SyncCacheContract.test.cpp, against both
// the real cache and FakeSyncCache). Uses tests/support/TempDbFile.h, which removes the file and its
// WAL/SHM siblings. SmatchetTests only, like the other file-backed LocalCacheManager suites.

#include "../support/TempDbFile.h"

#include "LocalCacheManager.h"
#include "PendingActionTypes.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <doctest/doctest.h>

#include <vector>

using smatchet_tests::TempDbFile;

namespace {

bool TableExists(SQLite::Database& db, const char* name) {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    q.bind(1, name);
    return q.executeStep() && q.getColumn(0).getInt() == 1;
}

} // namespace

TEST_SUITE("LocalCacheManagerPendingActionsPersist") {

    TEST_CASE("a queued comment survives closing and reopening the cache file") {
        TempDbFile tmp;
        std::int64_t id = 0;
        {
            LocalCacheManager mgr(tmp.Path());
            id = mgr.EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{\"body\":\"offline\",\"created\":1}",
                                          PendingActionState::kPending);
        }
        LocalCacheManager reopened(tmp.Path());
        const std::vector<PendingActionRecord> rows = reopened.LoadPendingActions();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].Id == id);
        CHECK(rows[0].State == PendingActionState::kPending);
        CHECK(rows[0].PayloadJson == "{\"body\":\"offline\",\"created\":1}");
    }

    TEST_CASE("a row left sending by an interrupted run opens as ambiguous; others keep their state") {
        TempDbFile tmp;
        {
            LocalCacheManager mgr(tmp.Path());
            const std::int64_t interrupted = mgr.EnqueuePendingAction("Jira", "comment_add", "PROJ-1",
                                                                      "{\"body\":\"a\"}", PendingActionState::kPending);
            mgr.UpdatePendingAction(interrupted, PendingActionState::kSending, 1, std::string());
            (void)mgr.EnqueuePendingAction("Jira", "comment_add", "PROJ-2", "{\"body\":\"b\"}",
                                           PendingActionState::kPending);
            (void)mgr.EnqueuePendingAction("Jira", "worklog_add", "PROJ-3", "{}", PendingActionState::kNeedsReview);
        }
        {
            // The run died long ago: its claim is older than any send could still be in flight.
            SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE);
            raw.exec("UPDATE pending_actions SET claimed_at = claimed_at - 100000 WHERE state = 'sending'");
        }
        LocalCacheManager reopened(tmp.Path());
        const std::vector<PendingActionRecord> rows = reopened.LoadPendingActions();
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].State == PendingActionState::kAmbiguous);
        CHECK(rows[0].Attempts == 1);
        CHECK(rows[0].ClaimedAtEpochSec == 0); // the stale claim is gone with the flip
        CHECK(rows[1].State == PendingActionState::kPending);
        CHECK(rows[2].State == PendingActionState::kNeedsReview);
    }

    TEST_CASE("a row `sending` with no claim (written before claims existed) opens as ambiguous") {
        TempDbFile tmp;
        {
            LocalCacheManager mgr(tmp.Path());
            (void)mgr.EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{}", PendingActionState::kSending);
        }
        LocalCacheManager reopened(tmp.Path());
        const std::vector<PendingActionRecord> rows = reopened.LoadPendingActions();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].State == PendingActionState::kAmbiguous);
    }

    TEST_CASE("another Smatchet's live claim survives this open, and is reported as foreign") {
        TempDbFile tmp;
        LocalCacheManager first(tmp.Path());
        const std::int64_t id =
            first.EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{}", PendingActionState::kPending);
        REQUIRE(first.TransitionPendingAction(id, PendingActionState::kPending, PendingActionState::kSending, 1, ""));
        {
            const std::vector<PendingActionRecord> own = first.LoadPendingActions();
            REQUIRE(own.size() == 1);
            CHECK(own[0].State == PendingActionState::kSending);
            CHECK(own[0].ClaimIsOwn);
            CHECK(own[0].ClaimedAtEpochSec > 0);
        }
        // A second process opens the same file while the first is mid-send: it must not flip the claim.
        LocalCacheManager second(tmp.Path());
        const std::vector<PendingActionRecord> seen = second.LoadPendingActions();
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].State == PendingActionState::kSending);
        CHECK_FALSE(seen[0].ClaimIsOwn);
        CHECK(seen[0].ClaimedAtEpochSec == first.LoadPendingActions()[0].ClaimedAtEpochSec);
        // Any other transition releases the claim.
        first.UpdatePendingAction(id, PendingActionState::kAmbiguous, 1, "lost response");
        const std::vector<PendingActionRecord> released = second.LoadPendingActions();
        REQUIRE(released.size() == 1);
        CHECK(released[0].ClaimedAtEpochSec == 0);
        CHECK_FALSE(released[0].ClaimIsOwn);
    }

    TEST_CASE("two processes that read the same expired claim cannot both take the row over") {
        TempDbFile tmp;
        LocalCacheManager first(tmp.Path());
        LocalCacheManager second(tmp.Path()); // opened before the row exists: nothing for its open to flip
        const std::int64_t id =
            first.EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{}", PendingActionState::kSending);
        // Both replay passes loaded the row as `sending` with the same (never-set) claim.
        const std::int64_t staleClaim = first.LoadPendingActions()[0].ClaimedAtEpochSec;
        REQUIRE(second.LoadPendingActions()[0].ClaimedAtEpochSec == staleClaim);
        const bool firstWon = first.TakeOverPendingActionClaim(id, staleClaim, 1, "");
        const bool secondWon = second.TakeOverPendingActionClaim(id, staleClaim, 1, "");
        CHECK(firstWon);
        CHECK_FALSE(secondWon); // the first's fresh claim is no longer the one the second read
        const PendingActionRecord seen = second.LoadPendingActions()[0];
        CHECK_FALSE(seen.ClaimIsOwn); // it is the first connection's claim
        CHECK(seen.ClaimedAtEpochSec > 0);
    }

    TEST_CASE(
        "a pending_actions table from before claims existed gains the claim columns, its rows read as unclaimed") {
        TempDbFile tmp;
        {
            // The pre-claims shape: the table exactly as it was, with a row mid-send.
            SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            raw.exec("CREATE TABLE pending_actions (id INTEGER PRIMARY KEY AUTOINCREMENT, backend_key TEXT NOT NULL "
                     "DEFAULT '', kind TEXT NOT NULL, issue_key TEXT NOT NULL, payload_json TEXT NOT NULL, state TEXT "
                     "NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_at "
                     "INTEGER NOT NULL)");
            raw.exec("INSERT INTO pending_actions (backend_key, kind, issue_key, payload_json, state, created_at) "
                     "VALUES ('Jira', 'comment_add', 'PROJ-1', '{}', 'sending', 1700000000)");
        }
        LocalCacheManager mgr(tmp.Path());
        const std::vector<PendingActionRecord> rows = mgr.LoadPendingActions();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].State == PendingActionState::kAmbiguous); // never claimed: old by any measure, so it flips
        CHECK(rows[0].ClaimedAtEpochSec == 0);
        CHECK_FALSE(rows[0].ClaimIsOwn);
    }

    TEST_CASE("a cache file from before the pending_actions tables existed gains them on open") {
        TempDbFile tmp;
        {
            LocalCacheManager mgr(tmp.Path());
            (void)mgr.EnqueuePendingAction("Jira", "comment_add", "PROJ-1", "{}", PendingActionState::kPending);
        }
        {
            // Simulate the pre-change file: every other table present, the pending_actions pair absent.
            SQLite::Database raw(tmp.Path(), SQLite::OPEN_READWRITE);
            raw.exec("DROP TABLE pending_actions");
            raw.exec("DROP TABLE pending_actions_dead");
            REQUIRE_FALSE(TableExists(raw, "pending_actions"));
        }
        LocalCacheManager reopened(tmp.Path());
        CHECK(reopened.LoadPendingActions().empty());
        CHECK(reopened.LoadDeadPendingActions().empty());
        const std::int64_t id =
            reopened.EnqueuePendingAction("Jira", "comment_add", "PROJ-9", "{}", PendingActionState::kPending);
        CHECK(id > 0);
        reopened.ArchivePendingAction(id, "replay_rejected", "x");
        CHECK(reopened.LoadDeadPendingActions().size() == 1);
    }
}
