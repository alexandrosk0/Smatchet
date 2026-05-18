#ifndef SMATCHET_AGENT_PROPOSAL_STORE_H
#define SMATCHET_AGENT_PROPOSAL_STORE_H

// AgentProposalStore — SQLite persistence for AgentProposal (T4 of the
// agentic-flow plan, decisions #4 + #14). Three tables:
//
//   agent_proposals   — one row per LLM-suggested action, lifecycle-tracked
//                       through AgentProposalState transitions.
//   agent_poll_cursor — durability for the GitHub poll cursor (per
//                       (source_tracker, repo_key) tuple) so a restart never
//                       re-triages the same issues.
//   schema_version    — single-row migration ledger; version 1 is the first
//                       shipped state of the agentic SQLite surface. Older
//                       databases (unversioned, created on a build before
//                       this row landed) auto-upgrade to version 1 on open
//                       since the agent_* tables are additive; the open
//                       path stamps version=1 idempotently and never
//                       double-bumps on subsequent re-opens.
//
// Migration policy: when an older shipped version exists, AgentProposalStore
// applies migrations in numeric order on open. Migrations are pure SQL +
// idempotent — re-running them must be safe. Version 1 has no migration body
// (it is the first recorded shipped state); future bumps add a step.

#include "AgentProposal.h"

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
    // body lands.
    static constexpr int kCurrentSchemaVersion = 1;

    // Returns the schema_version recorded in the database. Stamped at version
    // 1 on first open (whether the db is brand-new or an older unversioned
    // db) and never double-bumps on subsequent opens. Returns false + sets
    // outError on DB-read failure.
    bool GetSchemaVersion(int& outVersion, std::string& outError) const;

  private:
    // Creates the schema_version table if missing, inserts the current
    // shipped version on first call, applies any pending migrations in
    // numeric order. Idempotent: subsequent calls find the row already at
    // kCurrentSchemaVersion and return without mutation. Throws on
    // unrecoverable DB error so the open path surfaces the failure to the
    // caller via the std::runtime_error path.
    void EnsureSchemaVersion();

    std::unique_ptr<SQLite::Database> db_;
};

#endif // SMATCHET_AGENT_PROPOSAL_STORE_H
