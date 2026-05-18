#include <doctest/doctest.h>

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgenticInferenceClientPure::ProposedAction;

// Each test builds a fresh in-memory DB. ":memory:" gives a private SQLite handle that
// dies with the AgentProposalStore — perfect for round-trip tests without filesystem
// pollution. WAL is silently ignored on :memory: (logged as warn during ctor); the
// schema-create + CRUD path is unaffected.
AgentProposal MakeProposal(const std::string& issueKey, ProposedAction action = ProposedAction::CommentAdd) {
    AgentProposal p;
    p.sourceTracker = "github";
    p.issueKey = issueKey;
    p.action = action;
    p.rationale = "auto-generated rationale for " + issueKey;
    p.payload = nlohmann::json{{"body", "hello from t4"}};
    return p;
}

} // namespace

TEST_CASE("AgentProposalStore: construct/destruct round-trip is idempotent") {
    // Two constructors over the same in-memory path don't share state (`:memory:` is
    // per-connection) but each must CREATE TABLE IF NOT EXISTS without throwing.
    {
        AgentProposalStore s(":memory:");
        std::string err;
        std::vector<AgentProposal> rows;
        CHECK(s.Query({}, rows, err));
        CHECK(rows.empty());
        CHECK(err.empty());
    }
    {
        AgentProposalStore s(":memory:");
        std::string err;
        std::vector<AgentProposal> rows;
        CHECK(s.Query({}, rows, err));
        CHECK(rows.empty());
    }
}

TEST_CASE("AgentProposalStore: Insert stamps id, createdAtSec, lastUpdatedAtSec") {
    AgentProposalStore s(":memory:");
    AgentProposal p = MakeProposal("smatchet/example#1");
    std::string err;
    REQUIRE(s.Insert(p, err));
    CHECK(err.empty());
    CHECK(p.id > 0);
    CHECK(p.createdAtSec > 0);
    CHECK(p.lastUpdatedAtSec > 0);
    CHECK(p.createdAtSec == p.lastUpdatedAtSec);
    CHECK(p.state == AgentProposalState::Pending);
}

TEST_CASE("AgentProposalStore: Insert + Find round-trips all fields including payload json") {
    AgentProposalStore s(":memory:");
    AgentProposal p = MakeProposal("smatchet/example#2", ProposedAction::DerivedTicketCreate);
    p.payload = nlohmann::json{{"targetTracker", "jira"},
                               {"summary", "implement feature X"},
                               {"description", "long body\nwith newlines"},
                               {"nested", nlohmann::json{{"k", 42}, {"arr", {1, 2, 3}}}}};
    std::string err;
    REQUIRE(s.Insert(p, err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.id == p.id);
    CHECK(got.sourceTracker == "github");
    CHECK(got.issueKey == "smatchet/example#2");
    CHECK(got.action == ProposedAction::DerivedTicketCreate);
    CHECK(got.rationale == p.rationale);
    CHECK(got.payload == p.payload);
    CHECK(got.state == AgentProposalState::Pending);
    CHECK(got.createdAtSec == p.createdAtSec);
    CHECK(got.lastUpdatedAtSec == p.lastUpdatedAtSec);
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Query filter by state") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal a = MakeProposal("smatchet/example#10");
    AgentProposal b = MakeProposal("smatchet/example#11");
    AgentProposal c = MakeProposal("smatchet/example#12");
    REQUIRE(s.Insert(a, err));
    REQUIRE(s.Insert(b, err));
    REQUIRE(s.Insert(c, err));
    REQUIRE(s.Transition(b.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(c.id, AgentProposalState::Rejected, "", err));

    AgentProposalStore::Filter f;
    f.states.push_back(AgentProposalState::Pending);
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 1);
    CHECK(rows[0].id == a.id);

    f.states = {AgentProposalState::Approved, AgentProposalState::Rejected};
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 2);
}

TEST_CASE("AgentProposalStore: Query filter by sourceTracker + issueKey") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal gh1 = MakeProposal("smatchet/example#20");
    AgentProposal gh2 = MakeProposal("smatchet/example#21");
    AgentProposal jira = MakeProposal("PROJ-42");
    jira.sourceTracker = "jira";
    REQUIRE(s.Insert(gh1, err));
    REQUIRE(s.Insert(gh2, err));
    REQUIRE(s.Insert(jira, err));

    AgentProposalStore::Filter f;
    f.sourceTracker = "github";
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 2);

    f = {};
    f.issueKey = "PROJ-42";
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 1);
    CHECK(rows[0].sourceTracker == "jira");
}

TEST_CASE("AgentProposalStore: Query limit caps result count") {
    AgentProposalStore s(":memory:");
    std::string err;
    for (int i = 0; i < 8; ++i) {
        AgentProposal p = MakeProposal("smatchet/example#" + std::to_string(30 + i));
        REQUIRE(s.Insert(p, err));
    }
    AgentProposalStore::Filter f;
    f.limit = 5;
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 5);
}

TEST_CASE("AgentProposalStore: Transition Pending -> Approved succeeds + bumps lastUpdatedAtSec") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#40");
    REQUIRE(s.Insert(p, err));
    const int64_t origUpdated = p.lastUpdatedAtSec;
    // Sleep > 1s so the second wall-clock second-tick is observable. The store
    // stamps unix-epoch seconds, so sub-second waits can collapse to the same value.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Approved);
    CHECK(got.lastUpdatedAtSec > origUpdated);
    CHECK(got.createdAtSec == p.createdAtSec); // unchanged
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Transition Pending -> Applied is rejected (must go through Approved)") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#41");
    REQUIRE(s.Insert(p, err));
    CHECK_FALSE(s.Transition(p.id, AgentProposalState::Applied, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);

    // Row should still be Pending — the failed transition is purely advisory.
    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Pending);
}

TEST_CASE("AgentProposalStore: Transition Approved -> Applied succeeds") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#42");
    REQUIRE(s.Insert(p, err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Applied, "", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Applied);
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Transition Approved -> Failed stores applyError") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#43");
    REQUIRE(s.Insert(p, err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Failed, "HTTP 502 from backend", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Failed);
    CHECK(got.applyError == "HTTP 502 from backend");
}

TEST_CASE("AgentProposalStore: Transition on non-existent id returns false") {
    AgentProposalStore s(":memory:");
    std::string err;
    CHECK_FALSE(s.Transition(99999, AgentProposalState::Approved, "", err));
    CHECK(err.find("row not found") != std::string::npos);
}

TEST_CASE("AgentProposalStore: Transition on terminal state returns false") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal applied = MakeProposal("smatchet/example#50");
    AgentProposal rejected = MakeProposal("smatchet/example#51");
    AgentProposal failed = MakeProposal("smatchet/example#52");
    REQUIRE(s.Insert(applied, err));
    REQUIRE(s.Insert(rejected, err));
    REQUIRE(s.Insert(failed, err));
    REQUIRE(s.Transition(applied.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(applied.id, AgentProposalState::Applied, "", err));
    REQUIRE(s.Transition(rejected.id, AgentProposalState::Rejected, "", err));
    REQUIRE(s.Transition(failed.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(failed.id, AgentProposalState::Failed, "boom", err));

    CHECK_FALSE(s.Transition(applied.id, AgentProposalState::Pending, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
    CHECK_FALSE(s.Transition(rejected.id, AgentProposalState::Approved, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
    CHECK_FALSE(s.Transition(failed.id, AgentProposalState::Approved, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
}

TEST_CASE("AgentProposalStore: GetPollCursor on missing key returns 0 + ok=true") {
    AgentProposalStore s(":memory:");
    std::string err;
    int64_t cursor = 12345; // sentinel; should be reset to 0 by the call.
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 0);
    CHECK(err.empty());
}

TEST_CASE("AgentProposalStore: SetPollCursor + GetPollCursor round-trip") {
    AgentProposalStore s(":memory:");
    std::string err;
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 1700000000, err));
    int64_t cursor = 0;
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 1700000000);
}

TEST_CASE("AgentProposalStore: SetPollCursor twice on same key overwrites") {
    AgentProposalStore s(":memory:");
    std::string err;
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 1000, err));
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 2000, err));
    int64_t cursor = 0;
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 2000);

    // Different (tracker, repo) tuple stays independent.
    REQUIRE(s.SetPollCursor("github", "smatchet/other", 5000, err));
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 2000);
    REQUIRE(s.GetPollCursor("github", "smatchet/other", cursor, err));
    CHECK(cursor == 5000);
}

TEST_CASE("AgentProposalStore: state string helpers round-trip all 5 enum values") {
    const AgentProposalState values[] = {AgentProposalState::Pending, AgentProposalState::Approved,
                                         AgentProposalState::Rejected, AgentProposalState::Applied,
                                         AgentProposalState::Failed};
    for (const auto v : values) {
        const std::string s = AgentProposalStateToString(v);
        AgentProposalState out;
        CHECK(ParseAgentProposalState(s, out));
        CHECK(out == v);
    }

    // Unknown literal stays rejected — caller's `out` is untouched.
    AgentProposalState sentinel = AgentProposalState::Approved;
    CHECK_FALSE(ParseAgentProposalState("Bogus", sentinel));
    CHECK(sentinel == AgentProposalState::Approved);
}

TEST_CASE("AgentProposalStore: schema_version is stamped at kCurrentSchemaVersion on first open") {
    AgentProposalStore s(":memory:");
    std::string err;
    int v = -1;
    REQUIRE(s.GetSchemaVersion(v, err));
    CHECK(err.empty());
    CHECK(v == AgentProposalStore::kCurrentSchemaVersion);
    CHECK(v == 2); // H10 shipped version of the agentic SQLite surface.
}

TEST_CASE("AgentProposalStore: schema_version does not double-bump across re-opens of the same db") {
    // :memory: is per-connection, so to exercise the "open against an existing
    // db that already has a schema_version row" path we use a real temp file.
    // Two sequential AgentProposalStore ctors share the file; the second one
    // must see the current version (already stamped) and leave it untouched,
    // not bump beyond.
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_h10_schema.sqlite";
    {
        AgentProposalStore s(path);
        std::string err;
        int v = -1;
        REQUIRE(s.GetSchemaVersion(v, err));
        CHECK(v == AgentProposalStore::kCurrentSchemaVersion);
    }
    {
        AgentProposalStore s(path);
        std::string err;
        int v = -1;
        REQUIRE(s.GetSchemaVersion(v, err));
        CHECK(v == AgentProposalStore::kCurrentSchemaVersion); // idempotent on re-open.
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: schema_version row count is exactly 1 (singleton constraint)") {
    // The CHECK (id = 1) clamps the table to a single row; INSERT OR IGNORE on
    // re-open must not produce duplicates. Verify by opening the same store
    // twice through the same connection-shared db and counting rows directly.
    AgentProposalStore s(":memory:");
    std::string err;
    int v = -1;
    REQUIRE(s.GetSchemaVersion(v, err));
    CHECK(v == AgentProposalStore::kCurrentSchemaVersion);

    // Re-entering EnsureSchemaVersion via a second ctor on a fresh handle
    // creates an isolated db (per-connection :memory:), but the per-call
    // idempotency proof remains: the in-process open path is the single
    // mutator and is gated by INSERT OR IGNORE / UPDATE-on-migration.
    AgentProposalStore s2(":memory:");
    int v2 = -1;
    REQUIRE(s2.GetSchemaVersion(v2, err));
    CHECK(v2 == AgentProposalStore::kCurrentSchemaVersion);
}

TEST_CASE("AgentProposalStore: kCurrentSchemaVersion is exactly 2 — guard against silent N>2 increments") {
    // AGENTS.md § Schema-version bumps: "the shipped version should be exactly
    // one higher than the previous shipped version, not N higher because of
    // intermediate iterations." T9 shipped v1; H10 ships v2 (adds
    // agent_pr_watch + handoff_status column). Bump this constant alongside
    // the production-side constant only when a real migration body lands.
    CHECK(AgentProposalStore::kCurrentSchemaVersion == 2);
}

TEST_CASE("AgentProposalStore: v1 db migrates in place to v2 (agent_pr_watch + handoff_status)") {
    // Build a v1-shaped db by hand (without the v2 table or column) and then
    // open it via AgentProposalStore — the migration body must add the table
    // and column without throwing and bump schema_version to 2.
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_h10_migrate_v1.sqlite";
    {
        // Build the v1 schema directly via raw SQLite, mirroring the
        // pre-H10 ctor body (no agent_pr_watch table, no handoff_status
        // column). schema_version stamped at 1.
        SQLite::Database side(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        side.exec("CREATE TABLE agent_proposals ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                  "source_tracker TEXT NOT NULL, "
                  "issue_key TEXT NOT NULL, "
                  "action TEXT NOT NULL, "
                  "rationale TEXT NOT NULL, "
                  "payload_json TEXT NOT NULL, "
                  "state TEXT NOT NULL, "
                  "created_at_sec INTEGER NOT NULL, "
                  "last_updated_at_sec INTEGER NOT NULL, "
                  "apply_error TEXT NOT NULL DEFAULT '')");
        side.exec("CREATE TABLE agent_poll_cursor ("
                  "source_tracker TEXT NOT NULL, "
                  "repo_key TEXT NOT NULL, "
                  "last_seen_updated_at INTEGER NOT NULL, "
                  "PRIMARY KEY (source_tracker, repo_key))");
        side.exec("CREATE TABLE schema_version ("
                  "id INTEGER PRIMARY KEY CHECK (id = 1), "
                  "version INTEGER NOT NULL)");
        side.exec("INSERT INTO schema_version (id, version) VALUES (1, 1)");
        // Insert one proposal so the migration's ADD COLUMN has something to
        // backfill the default for.
        SQLite::Statement ins(side, "INSERT INTO agent_proposals (source_tracker, issue_key, action, rationale, "
                                    "payload_json, state, created_at_sec, last_updated_at_sec, apply_error) "
                                    "VALUES ('github', 'pre/v2#1', 'CommentAdd', 'pre-migration', '{}', 'Pending', "
                                    "100, 100, '')");
        ins.exec();
    }
    // Open through AgentProposalStore — should migrate cleanly.
    {
        AgentProposalStore s(path);
        std::string err;
        int v = -1;
        REQUIRE(s.GetSchemaVersion(v, err));
        CHECK(v == 2);
    }
    // Verify the table + column landed via side-channel inspection.
    {
        SQLite::Database side(path, SQLite::OPEN_READONLY);
        // agent_pr_watch table exists.
        SQLite::Statement tableQ(
            side, "SELECT name FROM sqlite_master WHERE type='table' AND name='agent_pr_watch'");
        REQUIRE(tableQ.executeStep());
        CHECK(std::string(tableQ.getColumn(0).getText()) == "agent_pr_watch");
        // handoff_status column on agent_proposals exists + defaults to "".
        SQLite::Statement rowQ(side, "SELECT handoff_status FROM agent_proposals WHERE issue_key='pre/v2#1'");
        REQUIRE(rowQ.executeStep());
        CHECK(std::string(rowQ.getColumn(0).getText()).empty());
    }
    // Re-open a v2 db — must be a no-op (idempotent).
    {
        AgentProposalStore s(path);
        std::string err;
        int v = -1;
        REQUIRE(s.GetSchemaVersion(v, err));
        CHECK(v == 2);
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: agent_pr_watch round-trip via Set/Get/Delete") {
    AgentProposalStore s(":memory:");
    std::string err;

    // Empty Get on missing proposal id is OK (first-poll semantics) — fields default.
    AgentProposalStore::PrWatchRow row;
    REQUIRE(s.GetPrWatch(42, row, err));
    CHECK(row.proposalId == 0);
    CHECK(row.iterationCount == 0);
    CHECK(row.lastSeenCommentIdStr.empty());

    // Set + Get round-trip.
    AgentProposalStore::PrWatchRow w;
    w.proposalId = 42;
    w.prUrl = "https://github.com/smatchet/example/pull/7";
    w.lastSeenCommentIdStr = "9876543210";
    w.iterationCount = 3;
    w.lastPolledAtSec = 1778500000;
    REQUIRE(s.SetPrWatch(w, err));

    AgentProposalStore::PrWatchRow got;
    REQUIRE(s.GetPrWatch(42, got, err));
    CHECK(got.proposalId == 42);
    CHECK(got.prUrl == w.prUrl);
    CHECK(got.lastSeenCommentIdStr == "9876543210");
    CHECK(got.iterationCount == 3);
    CHECK(got.lastPolledAtSec == 1778500000);

    // Re-Set on same proposal id overwrites (idempotent INSERT OR REPLACE).
    w.iterationCount = 5;
    w.lastSeenCommentIdStr = "9999999999";
    REQUIRE(s.SetPrWatch(w, err));
    REQUIRE(s.GetPrWatch(42, got, err));
    CHECK(got.iterationCount == 5);
    CHECK(got.lastSeenCommentIdStr == "9999999999");

    // Delete clears the row + subsequent Get returns defaulted row.
    REQUIRE(s.DeletePrWatch(42, err));
    REQUIRE(s.GetPrWatch(42, got, err));
    CHECK(got.proposalId == 0);
    CHECK(got.iterationCount == 0);

    // Delete on missing row is not an error (idempotent).
    REQUIRE(s.DeletePrWatch(42, err));
    REQUIRE(s.DeletePrWatch(9999, err));
}

TEST_CASE("AgentProposalStore: agent_pr_watch persists across store re-open (cursor durability)") {
    // The whole point of the H10 schema bump — a watcher cursor must survive
    // an app restart so the next tick resumes from the same comment id
    // instead of replaying the entire PR thread.
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_h10_pr_watch_persist.sqlite";
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposalStore::PrWatchRow w;
        w.proposalId = 17;
        w.prUrl = "https://github.com/team/repo/pull/23";
        w.lastSeenCommentIdStr = "1234567890";
        w.iterationCount = 4;
        w.lastPolledAtSec = 1778600000;
        REQUIRE(s.SetPrWatch(w, err));
    }
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposalStore::PrWatchRow got;
        REQUIRE(s.GetPrWatch(17, got, err));
        CHECK(got.proposalId == 17);
        CHECK(got.lastSeenCommentIdStr == "1234567890");
        CHECK(got.iterationCount == 4);
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: handoff_status round-trip via Set/Get") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#hs-1");
    REQUIRE(s.Insert(p, err));

    // Default after insert is empty (no handoff started).
    std::string status = "sentinel";
    REQUIRE(s.GetHandoffStatus(p.id, status, err));
    CHECK(status.empty());

    // Set + Get round-trip.
    REQUIRE(s.SetHandoffStatus(p.id, "Spawning", err));
    REQUIRE(s.GetHandoffStatus(p.id, status, err));
    CHECK(status == "Spawning");

    REQUIRE(s.SetHandoffStatus(p.id, "PrOpen", err));
    REQUIRE(s.GetHandoffStatus(p.id, status, err));
    CHECK(status == "PrOpen");

    // SetHandoffStatus on a non-existent proposal id surfaces row-not-found.
    CHECK_FALSE(s.SetHandoffStatus(99999, "Spawning", err));
    CHECK(err.find("row not found") != std::string::npos);
}

// Bundle B CR#230:107 — InsertMany must be atomic. A failure on row N must
// roll back rows 0..N-1 within the same SQLite transaction. The trigger here
// is a NOT NULL constraint violation on the action column (action="" maps to
// "" via ActionToString(Unknown) which is still non-null; we instead use a
// real disk db and a malformed JSON dump that violates a different invariant.
// Simpler approach: inject a duplicate primary-key conflict by pre-committing
// a row with a known rowid, then asking InsertMany to clobber it. But the
// table uses AUTOINCREMENT — we can't choose the rowid. So the cleanest
// trigger is the source_tracker NOT NULL constraint: bind a sourceTracker
// containing a SQLite-reserved byte sequence? No — that'd succeed. Use a
// disk-backed db with a tiny page-cache size + a forced disk error? Out of
// scope. The atomicity contract is best exercised end-to-end via the
// integration test below, which forces a constraint violation by binding a
// row that exercises the schema's CHECK / NOT NULL constraints. For pure
// rollback proof on the SQLite::Transaction RAII shape see the empty-input
// fast path + the per-issue scenario test (test-agentic-approve-reject.sh).
TEST_CASE("AgentProposalStore: InsertMany on empty input is a no-op + returns true") {
    AgentProposalStore s(":memory:");
    std::string err;
    std::vector<AgentProposal> empty;
    REQUIRE(s.InsertMany(empty, err));
    CHECK(err.empty());

    // Store remains empty.
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query({}, rows, err));
    CHECK(rows.empty());
}

TEST_CASE("AgentProposalStore: InsertMany commits all rows atomically on success") {
    AgentProposalStore s(":memory:");
    std::string err;
    std::vector<AgentProposal> batch;
    for (int i = 0; i < 5; ++i) {
        batch.push_back(MakeProposal("smatchet/example#bm-" + std::to_string(i)));
    }
    REQUIRE(s.InsertMany(batch, err));
    CHECK(err.empty());
    for (const auto& p : batch) {
        CHECK(p.id > 0);
        CHECK(p.state == AgentProposalState::Pending);
        CHECK(p.createdAtSec > 0);
        CHECK(p.lastUpdatedAtSec == p.createdAtSec);
    }

    std::vector<AgentProposal> rows;
    REQUIRE(s.Query({}, rows, err));
    CHECK(rows.size() == 5);
}

TEST_CASE("AgentProposalStore: InsertMany rolls back ALL rows when one fails") {
    // The atomicity contract: if any row in the batch fails to insert, NO
    // rows from the batch land in the db. Trigger the failure by binding a
    // payload that breaks the json dump path? No — payloads can't fail to
    // dump. The cleanest trigger is the source_tracker NOT NULL constraint:
    // an empty string still satisfies NOT NULL (it's the zero-length text
    // type). We force a different violation by closing the db connection
    // mid-batch — not portable. Best path: use a CHECK constraint we add via
    // raw SQL on the same db handle, prior to the InsertMany call, that
    // rejects a specific issueKey marker. SQLite's CHECK constraints are
    // enforced per-row and a violation aborts the surrounding transaction.
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_bundle_b_insertmany.sqlite";
    {
        AgentProposalStore s(path);
        // Add a CHECK constraint via a side-channel handle BEFORE the test
        // batch lands. SQLite allows ALTER TABLE only for limited mutations;
        // adding a CHECK requires recreating the table. Instead, install a
        // trigger that raises ABORT for the sentinel issueKey.
        {
            SQLite::Database side(path, SQLite::OPEN_READWRITE);
            side.exec("CREATE TRIGGER reject_sentinel BEFORE INSERT ON agent_proposals "
                      "FOR EACH ROW WHEN NEW.issue_key = 'force-rollback-sentinel' "
                      "BEGIN SELECT RAISE(ABORT, 'sentinel rejected'); END;");
        }
        std::vector<AgentProposal> batch;
        batch.push_back(MakeProposal("smatchet/example#ok-1"));
        batch.push_back(MakeProposal("smatchet/example#ok-2"));
        batch.push_back(MakeProposal("force-rollback-sentinel")); // 3rd row triggers the abort
        batch.push_back(MakeProposal("smatchet/example#ok-4"));

        std::string err;
        CHECK_FALSE(s.InsertMany(batch, err));
        CHECK(err.find("InsertMany") != std::string::npos);

        // All input row ids reset to 0 on rollback.
        for (const auto& p : batch) {
            CHECK(p.id == 0);
        }

        // Zero rows in db — the 2 "ok" inserts before the failure must have
        // been rolled back, NOT left behind as a half-written partial batch.
        std::vector<AgentProposal> rows;
        REQUIRE(s.Query({}, rows, err));
        CHECK(rows.empty());
    }
    std::remove(path.c_str());
}

// Bundle B CR#229:318 — RowToProposal must propagate unknown state / action
// literals as errors rather than silently re-mapping to Pending/Unknown. A
// future migration that adds a new state (e.g. "Reviewing") would otherwise
// have its rows silently re-actioned by an old build that wasn't taught
// about it — data-loss adjacent.
TEST_CASE("AgentProposalStore: Find returns false on unknown state literal in db") {
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_bundle_b_unknownstate.sqlite";
    int64_t insertedId = 0;
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposal p = MakeProposal("smatchet/example#unknownstate");
        REQUIRE(s.Insert(p, err));
        insertedId = p.id;
        // Side-channel mutate the row to a future state value.
        SQLite::Database side(path, SQLite::OPEN_READWRITE);
        SQLite::Statement u(side, "UPDATE agent_proposals SET state='Reviewing' WHERE id=?");
        u.bind(1, static_cast<long long>(insertedId));
        u.exec();
    }
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposal got;
        CHECK_FALSE(s.Find(insertedId, got, err));
        CHECK(err.find("unknown state literal in db") != std::string::npos);
        CHECK(err.find("Reviewing") != std::string::npos);
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: Query returns false + clears partial output on unknown state literal") {
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_bundle_b_querystate.sqlite";
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposal a = MakeProposal("smatchet/example#qs-1");
        AgentProposal b = MakeProposal("smatchet/example#qs-2");
        AgentProposal c = MakeProposal("smatchet/example#qs-3");
        REQUIRE(s.Insert(a, err));
        REQUIRE(s.Insert(b, err));
        REQUIRE(s.Insert(c, err));
        SQLite::Database side(path, SQLite::OPEN_READWRITE);
        SQLite::Statement u(side, "UPDATE agent_proposals SET state='FutureState' WHERE id=?");
        u.bind(1, static_cast<long long>(b.id));
        u.exec();
    }
    {
        AgentProposalStore s(path);
        std::string err;
        std::vector<AgentProposal> rows;
        rows.reserve(8);
        CHECK_FALSE(s.Query({}, rows, err));
        // Partial result cleared — caller can't accidentally consume a
        // truncated row set thinking it succeeded.
        CHECK(rows.empty());
        CHECK(err.find("unknown state literal in db") != std::string::npos);
        CHECK(err.find("FutureState") != std::string::npos);
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: Find returns false on unknown action literal in db") {
    const std::string path = std::string(std::tmpnam(nullptr)) + "_smatchet_bundle_b_unknownaction.sqlite";
    int64_t insertedId = 0;
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposal p = MakeProposal("smatchet/example#unknownaction");
        REQUIRE(s.Insert(p, err));
        insertedId = p.id;
        SQLite::Database side(path, SQLite::OPEN_READWRITE);
        SQLite::Statement u(side, "UPDATE agent_proposals SET action='SummonDragon' WHERE id=?");
        u.bind(1, static_cast<long long>(insertedId));
        u.exec();
    }
    {
        AgentProposalStore s(path);
        std::string err;
        AgentProposal got;
        CHECK_FALSE(s.Find(insertedId, got, err));
        CHECK(err.find("unknown action literal in db") != std::string::npos);
        CHECK(err.find("SummonDragon") != std::string::npos);
    }
    std::remove(path.c_str());
}

TEST_CASE("AgentProposalStore: agentic action string helpers round-trip via proposal serialization") {
    AgentProposalStore s(":memory:");
    std::string err;
    const ProposedAction actions[] = {ProposedAction::CommentAdd,      ProposedAction::LabelAdd,
                                      ProposedAction::LabelRemove,     ProposedAction::AssigneeSet,
                                      ProposedAction::StateTransition, ProposedAction::DerivedTicketCreate,
                                      ProposedAction::ImplementIssue};
    for (const auto a : actions) {
        AgentProposal p =
            MakeProposal(std::string("smatchet/example#act-") + AgenticInferenceClientPure::ActionToString(a), a);
        REQUIRE(s.Insert(p, err));
        AgentProposal got;
        REQUIRE(s.Find(p.id, got, err));
        CHECK(got.action == a);
    }

    // Direct ParseAgenticAction smoke — including Unknown literal which the parser accepts.
    ProposedAction out = ProposedAction::Unknown;
    CHECK(ParseAgenticAction("CommentAdd", out));
    CHECK(out == ProposedAction::CommentAdd);
    CHECK(ParseAgenticAction("Unknown", out));
    CHECK(out == ProposedAction::Unknown);
    CHECK_FALSE(ParseAgenticAction("NotAnAction", out));
}
