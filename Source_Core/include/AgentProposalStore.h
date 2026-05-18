#ifndef SMATCHET_AGENT_PROPOSAL_STORE_H
#define SMATCHET_AGENT_PROPOSAL_STORE_H

// AgentProposalStore — SQLite persistence for AgentProposal (T4 of the
// agentic-flow plan, decisions #4 + #14). Two tables:
//
//   agent_proposals   — one row per LLM-suggested action, lifecycle-tracked
//                       through AgentProposalState transitions.
//   agent_poll_cursor — durability for the GitHub poll cursor (per
//                       (source_tracker, repo_key) tuple) so a restart never
//                       re-triages the same issues.
//
// Schema-version bump deferred to T9 per plan decision #14 — the tables are
// created via CREATE TABLE IF NOT EXISTS so they coexist with the existing
// schema without touching the cache_meta version row.

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

  private:
    std::unique_ptr<SQLite::Database> db_;
};

#endif // SMATCHET_AGENT_PROPOSAL_STORE_H
