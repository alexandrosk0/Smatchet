#ifndef SMATCHET_AGENT_PROPOSAL_STORE_H
#define SMATCHET_AGENT_PROPOSAL_STORE_H

// AgentProposalStore — SQLite persistence for AgentProposal (per
// agentic-flow plan decisions #4 + #14). Four tables (post-H10):
//
//   agent_proposals   — one row per LLM-suggested action, lifecycle-tracked
//                       through AgentProposalState transitions. Schema v2
//                       adds the `handoff_status` column so the proposal
//                       row surfaces the in-flight / terminal handoff state
//                       to the AgentProposalsUi panel without a join into
//                       the in-memory handoff table.
//   agent_poll_cursor — durability for the GitHub poll cursor (per
//                       (source_tracker, repo_key) tuple) so a restart never
//                       re-triages the same issues.
//   agent_pr_watch    — H10. Per-proposal PR-iteration cursor state — last
//                       seen PR comment id + iteration count + last-poll
//                       wall-clock. Persists `PrCommentWatcher` state across
//                       restarts (H7 left it in-memory); a re-armed handoff
//                       resumes from the same cursor instead of replaying
//                       every comment from the PrOpen transition forward.
//   schema_version    — single-row migration ledger; version 1 was the first
//                       shipped state of the agentic SQLite surface (T9).
//                       Version 2 (H10) adds `agent_pr_watch` + the
//                       `agent_proposals.handoff_status` column; the open
//                       path migrates v1 dbs in place. New dbs are stamped
//                       at the current version directly.
//
// Migration policy: when an older shipped version exists, AgentProposalStore
// applies migrations in numeric order on open. Migrations are pure SQL +
// idempotent — re-running them must be safe. Each version's body MUST be
// safe to re-run on a db that has already been bumped (defense in depth
// against partial-apply recovery).

#include "AgentProposal.h"
#include "AgenticInferenceClientPure.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SQLite {
class Database;
}

class AgentProposalStore {
  public:
    // Opens or creates the SQLite database at `dbPath`. Creates the two tables
    // (and their indexes) if they don't already exist. Idempotent across
    // repeated invocations on the same path. Throws std::runtime_error on
    // unrecoverable DB-open failure; table-create errors propagate via the
    // returned bool on subsequent operations.
    explicit AgentProposalStore(const std::string& dbPath);
    ~AgentProposalStore();

    AgentProposalStore(const AgentProposalStore&) = delete;
    AgentProposalStore& operator=(const AgentProposalStore&) = delete;

    // Inserts `proposal` as a new Pending row. Stamps createdAtSec +
    // lastUpdatedAtSec from the current wall clock and writes the new ROWID
    // back into `proposal.id`. The caller's other fields (sourceTracker,
    // issueKey, action, rationale, payload) are persisted verbatim.
    // Returns false on DB error; outError carries the reason.
    bool Insert(AgentProposal& proposal, std::string& outError);

    // Bundle B CR#230:107 — atomic batch insert. All proposals in the vector
    // commit together or none of them do; an insert failure on row N rolls
    // back rows 0..N-1 within the same SQLite transaction. Used by
    // AgenticTriageController::TriageIssue so a partial-insert failure (e.g.
    // disk full mid-batch) never leaves a fragmentary proposal set visible to
    // the UI. Each successful row has its ROWID + timestamps stamped back into
    // the corresponding element of `proposals`. Empty input returns true with
    // no DB writes.
    bool InsertMany(std::vector<AgentProposal>& proposals, std::string& outError);

    // Transitions the row identified by `id` to `newState`. Bumps
    // lastUpdatedAtSec. Stores `applyError` when `newState == Failed` (empty
    // string otherwise — ignored for non-Failed transitions).
    //
    // State machine — see AgentProposal.h for the canonical table:
    //   Pending  -> Approved | Rejected
    //   Approved -> Applied  | Failed
    //   Rejected, Applied, Failed are terminal.
    //
    // Returns false on (a) row not found, (b) invalid transition, (c) DB
    // error; outError describes which.
    bool Transition(int64_t id, AgentProposalState newState, const std::string& applyError, std::string& outError);

    // Bulk query with simple AND-conjunctive filters. Empty fields disable
    // their respective predicates; `limit == 0` disables the SQL LIMIT.
    struct Filter {
        std::vector<AgentProposalState> states;
        std::string sourceTracker;
        std::string issueKey;
        int limit = 0;
    };
    // Hard cap on Filter::states entries observed by Query. The vector is used
    // verbatim to build a SQL `IN (?, ?, ...)` list; without a cap a hostile
    // caller could submit an unbounded list and force SQLite to compile a
    // pathologically large statement. The 5-state enum lives well below this
    // ceiling so legitimate callers never trip it.
    static constexpr std::size_t kMaxFilterStates = 32;
    bool Query(const Filter& f, std::vector<AgentProposal>& out, std::string& outError) const;

    // Single-row lookup by ROWID. Returns false + outError if the row is
    // missing or the DB read fails.
    bool Find(int64_t id, AgentProposal& out, std::string& outError) const;

    // Poll-cursor helpers (decision #4). The (sourceTracker, repoKey) tuple is
    // the composite PRIMARY KEY of the agent_poll_cursor table. First-call
    // semantics: when no row exists, GetPollCursor returns true + writes 0 to
    // outLastSeenUpdatedAt so callers can treat "never polled" identically to
    // "polled at epoch 0" without an extra ok-but-empty signal.
    bool GetPollCursor(const std::string& sourceTracker, const std::string& repoKey, int64_t& outLastSeenUpdatedAt,
                       std::string& outError) const;

    // Inserts or replaces the (sourceTracker, repoKey) cursor row. Idempotent;
    // calling twice with the same key overwrites the prior value.
    bool SetPollCursor(const std::string& sourceTracker, const std::string& repoKey, int64_t lastSeenUpdatedAt,
                       std::string& outError);

    // The shipped schema version. Bumped exactly once per shipped feature
    // increment (AGENTS.md § Schema-version bumps). Increment in lock-step
    // with EnsureSchemaVersion's migration ladder whenever a real migration
    // body lands. H10 ships v2 (adds `agent_pr_watch` + handoff_status).
    static constexpr int kCurrentSchemaVersion = 2;

    // Returns the schema_version recorded in the database. Stamped at the
    // current shipped version on first open (whether the db is brand-new or
    // an older unversioned db) and migrated forward in-place for v1 dbs that
    // have not yet been bumped. Idempotent on already-current dbs. Returns
    // false + sets outError on DB-read failure.
    bool GetSchemaVersion(int& outVersion, std::string& outError) const;

    // ─── H10 agent_pr_watch persistence ───────────────────────────────────
    //
    // The H7 `PrCommentWatcher` cursor + iteration count must survive an
    // app restart so a half-processed PR comment thread does not double-fire
    // a respawn (or, worse, miss every comment between restarts). The
    // `agent_pr_watch` table mirrors the in-memory fields on
    // `AgenticHandoffController::ActiveHandoff` that the watcher reads on
    // every Tick: the last seen comment id (string — GitHub's `id` is a 64-
    // bit int that we round-trip as decimal text to avoid signed-overflow
    // ambiguity), the iteration count, and the wall-clock of the last
    // successful poll.

    struct PrWatchRow {
        std::int64_t proposalId = 0;
        std::string prUrl;
        std::string lastSeenCommentIdStr; // stringified GitHub comment id, "" = never seen
        int iterationCount = 0;
        std::int64_t lastPolledAtSec = 0; // 0 = never polled
    };

    // Upsert the PR-watch row for `proposalId`. Idempotent — calling twice
    // overwrites the prior row. Returns false + outError on DB error.
    bool SetPrWatch(const PrWatchRow& row, std::string& outError);

    // Reads the PR-watch row for `proposalId`. Returns true + populates
    // `outRow` when a row exists; returns true + leaves `outRow` defaulted
    // when no row exists (first-poll semantics — same shape as
    // GetPollCursor). Returns false on DB error.
    bool GetPrWatch(std::int64_t proposalId, PrWatchRow& outRow, std::string& outError) const;

    // Deletes the PR-watch row for `proposalId`. Idempotent — missing row
    // is not an error. Called when a handoff terminates (Complete / Failed
    // / Cancelled) so the table does not grow unbounded.
    bool DeletePrWatch(std::int64_t proposalId, std::string& outError);

    // ─── H10 agent_proposals.handoff_status column ────────────────────────
    //
    // Mirrors `CodingHarness::RunState` as a string so the
    // AgentProposalsUi panel can surface the in-flight handoff state on the
    // proposal row without joining against the in-memory controller table.
    // Empty string = no handoff started. Updated by
    // `AgenticHandoffController` after each FSM transition.

    bool SetHandoffStatus(std::int64_t proposalId, const std::string& status, std::string& outError);
    bool GetHandoffStatus(std::int64_t proposalId, std::string& outStatus, std::string& outError) const;

    // H9 — cross-flow notification hook. Fired by Transition() each time a
    // row's state advances Pending -> Approved AND the row's UPDATE commits
    // successfully. Used by AppController to invoke the AgenticHandoffController
    // when the approved row carries `action == ImplementIssue` and the
    // `HandoffAutoStartOnApprove` cfg is true. The callback runs inline on the
    // calling thread (the panel's worker-thread task), AFTER the SQLite
    // transaction commits — so receivers may safely re-read the row through
    // the store. Receiver must be fast: any blocking work (Start() invokes
    // git-worktree + harness spawn) must be punted via `LaunchBackgroundTask`,
    // not run inline.
    //
    // The action enum lives in the signature so a receiver can early-out on
    // non-ImplementIssue rows without an extra Find() round-trip. Pending ->
    // Rejected / Approved -> Applied / etc transitions never invoke the
    // callback — only the Pending -> Approved edge does.
    //
    // Single-subscriber for H9 (last setter wins); upgrade to multi-listener
    // when a second consumer materialises. Reset by passing an empty function.
    using OnApprovedFn = std::function<void(std::int64_t proposalId,
                                            AgenticInferenceClientPure::ProposedAction action)>;
    void SetOnProposalApproved(OnApprovedFn cb);

  private:
    // Creates the schema_version table if missing, inserts the current
    // shipped version on first call, applies any pending migrations in
    // numeric order. Idempotent: subsequent calls find the row already at
    // kCurrentSchemaVersion and return without mutation. Throws on
    // unrecoverable DB error so the open path surfaces the failure to the
    // caller via the std::runtime_error path.
    void EnsureSchemaVersion();

    std::unique_ptr<SQLite::Database> db_;

    // H9 — see SetOnProposalApproved. Empty by default; single-subscriber.
    // Not protected by an internal lock — callers set it once at AppController
    // init (before any Transition() can fire) and the function-object is
    // thread-safe to read after that point.
    OnApprovedFn onApproved_;
};

#endif // SMATCHET_AGENT_PROPOSAL_STORE_H
