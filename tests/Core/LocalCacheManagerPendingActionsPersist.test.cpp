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
        LocalCacheManager reopened(tmp.Path());
        const std::vector<PendingActionRecord> rows = reopened.LoadPendingActions();
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].State == PendingActionState::kAmbiguous);
        CHECK(rows[0].Attempts == 1);
        CHECK(rows[1].State == PendingActionState::kPending);
        CHECK(rows[2].State == PendingActionState::kNeedsReview);
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
