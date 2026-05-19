#include "AgentProposalStore.h"

#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

int64_t NowEpochSec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Mirrors the helper in `LocalCacheManager.cpp` — used by the v1 -> v2
// migration to add `handoff_status` only when it does not already exist
// (ALTER TABLE ADD COLUMN throws if the column is already present).
bool SqliteTableHasColumn(const SQLite::Database& db, const char* table, const char* col) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    SQLite::Statement q(db, sql);
    while (q.executeStep()) {
        if (std::strcmp(q.getColumn(1).getText(), col) == 0) {
            return true;
        }
    }
    return false;
}

// State machine: terminal Rejected/Applied/Failed never transitions out;
// re-attempts create a NEW Pending row rather than reviving a terminal one.
// Keeps the audit trail linear and prevents an Applied row from sliding back
// to Pending if the LLM later proposes the same action again.
bool IsLegalTransition(AgentProposalState from, AgentProposalState to) {
    switch (from) {
    case AgentProposalState::Pending:
        return to == AgentProposalState::Approved || to == AgentProposalState::Rejected;
    case AgentProposalState::Approved:
        return to == AgentProposalState::Applied || to == AgentProposalState::Failed;
    case AgentProposalState::Rejected:
    case AgentProposalState::Applied:
    case AgentProposalState::Failed:
        return false;
    }
    return false;
}

// Bundle B CR#229:318 — return bool so callers can surface schema-drift on
// unknown enum literals rather than silently re-classifying them as
// Pending/Unknown. A future migration that introduces a new state (e.g.
// "Reviewing") must not be re-actioned by an old build that wasn't taught
// about it; failing the row read is the safe default.
bool RowToProposal(SQLite::Statement& q, AgentProposal& out, std::string& outError) {
    out.id = q.getColumn(0).getInt64();
    out.sourceTracker = q.getColumn(1).getText();
    out.issueKey = q.getColumn(2).getText();
    const std::string actionStr = q.getColumn(3).getText();
    if (!ParseAgenticAction(actionStr, out.action)) {
        LOG_WARN("AgentProposalStore: unknown action literal in db (row id=%lld): %s",
                 static_cast<long long>(out.id), actionStr.c_str());
        outError = std::string("unknown action literal in db: ") + actionStr;
        return false;
    }
    out.rationale = q.getColumn(4).getText();
    const std::string payloadStr = q.getColumn(5).getText();
    try {
        out.payload = payloadStr.empty() ? nlohmann::json::object() : nlohmann::json::parse(payloadStr);
    } catch (const nlohmann::json::parse_error& e) {
        LOG_WARN("AgentProposalStore: payload json parse failed (row id=%lld): %s", static_cast<long long>(out.id),
                 e.what());
        out.payload = nlohmann::json::object();
    }
    const std::string stateStr = q.getColumn(6).getText();
    if (!ParseAgentProposalState(stateStr, out.state)) {
        LOG_WARN("AgentProposalStore: unknown state literal in db (row id=%lld): %s", static_cast<long long>(out.id),
                 stateStr.c_str());
        outError = std::string("unknown state literal in db: ") + stateStr;
        return false;
    }
    out.createdAtSec = q.getColumn(7).getInt64();
    out.lastUpdatedAtSec = q.getColumn(8).getInt64();
    out.applyError = q.getColumn(9).getText();
    return true;
}

} // namespace

// C++14 odr-use definition for the inline constexpr declared in the header.
// C++17 deprecated the redundancy; until the codebase moves off C++14 (Unreal
// compat — see AGENTS.md § Project rules) any caller that binds a reference
// or takes the address of the constant needs this definition to satisfy the
// linker.
constexpr int AgentProposalStore::kCurrentSchemaVersion;
constexpr std::size_t AgentProposalStore::kMaxFilterStates;

AgentProposalStore::AgentProposalStore(const std::string& dbPath)
    : db_(std::unique_ptr<SQLite::Database>(
          new SQLite::Database(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX))) {
    LOG_INFO("AgentProposalStore: opening db path=%s", dbPath.c_str());
    try {
        // Mirror LocalCacheManager pragmas — WAL + NORMAL is the recommended pairing for
        // multi-reader/single-writer workloads. The in-memory `:memory:` path silently
        // ignores WAL but the pragma itself is harmless.
        db_->exec("PRAGMA journal_mode=WAL");
        db_->exec("PRAGMA synchronous=NORMAL");
        db_->setBusyTimeout(5000);
    } catch (const std::exception& ex) {
        LOG_WARN("AgentProposalStore: failed to set WAL pragmas: %s", ex.what());
    }

    db_->exec("CREATE TABLE IF NOT EXISTS agent_proposals ("
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
    db_->exec("CREATE INDEX IF NOT EXISTS idx_agent_proposals_state "
              "ON agent_proposals(state)");
    db_->exec("CREATE INDEX IF NOT EXISTS idx_agent_proposals_issue_key "
              "ON agent_proposals(issue_key)");

    db_->exec("CREATE TABLE IF NOT EXISTS agent_poll_cursor ("
              "source_tracker TEXT NOT NULL, "
              "repo_key TEXT NOT NULL, "
              "last_seen_updated_at INTEGER NOT NULL, "
              "PRIMARY KEY (source_tracker, repo_key))");

    // H10 — agent_pr_watch table. Mirrors PrCommentWatcher cursor state so a
    // restart resumes the watch from the same comment id + iteration count
    // instead of replaying every comment from the PrOpen transition forward.
    // `proposal_id` is the PK because the watcher is per-handoff and at most
    // one PR is open per proposal at a time.
    db_->exec("CREATE TABLE IF NOT EXISTS agent_pr_watch ("
              "proposal_id INTEGER NOT NULL, "
              "pr_url TEXT NOT NULL, "
              "last_seen_comment_id_str TEXT NOT NULL DEFAULT '', "
              "iteration_count INTEGER NOT NULL DEFAULT 0, "
              "last_polled_at_sec INTEGER NOT NULL DEFAULT 0, "
              "PRIMARY KEY (proposal_id))");

    // coderabbit-react phase-1 — agent_open_pr_watch table. Sibling to
    // agent_pr_watch but keyed on `pr_url` because the registrar walks every
    // open PR on the configured base branch (not only handoff-spawned PRs).
    // Additive in v2 — no schema-version bump (this is a v2-extending CREATE
    // TABLE IF NOT EXISTS, not a migration body). Belt-and-braces idempotent.
    db_->exec("CREATE TABLE IF NOT EXISTS agent_open_pr_watch ("
              "pr_url TEXT NOT NULL, "
              "pr_number INTEGER NOT NULL, "
              "head_ref_name TEXT NOT NULL DEFAULT '', "
              "head_sha TEXT NOT NULL DEFAULT '', "
              "last_seen_comment_id_str TEXT NOT NULL DEFAULT '', "
              "last_seen_check_run_id INTEGER NOT NULL DEFAULT 0, "
              "iteration_count INTEGER NOT NULL DEFAULT 0, "
              "last_polled_at_sec INTEGER NOT NULL DEFAULT 0, "
              "PRIMARY KEY (pr_url))");

    // Records the shipped schema version. AGENTS.md § Schema-version bumps
    // requires exactly one shipped increment per feature increment; v1 shipped
    // at T9, v2 ships here at H10 (agent_pr_watch + handoff_status column).
    EnsureSchemaVersion();

    // H10 — the migration ladder runs BEFORE this point (EnsureSchemaVersion
    // owns ALTER TABLE handoff_status). For NEW dbs the CREATE TABLE IF NOT
    // EXISTS path above does NOT carry the handoff_status column (it was
    // shipped as an ALTER, not a CREATE column, to keep the v1 schema text
    // unchanged for archived state-replay tooling). We ensure the column
    // exists here as a belt-and-braces idempotent guard so a fresh-db open
    // path leaves the schema identical to a migrated-from-v1 path.
    if (!SqliteTableHasColumn(*db_, "agent_proposals", "handoff_status")) {
        db_->exec("ALTER TABLE agent_proposals ADD COLUMN handoff_status TEXT NOT NULL DEFAULT ''");
    }

    LOG_INFO("AgentProposalStore: schema ready (version=%d)", kCurrentSchemaVersion);
}

void AgentProposalStore::EnsureSchemaVersion() {
    // The schema_version table is a single-row ledger keyed implicitly by the
    // singleton constraint: a CHECK clamps id to 1 so callers can SELECT/UPDATE
    // without juggling row identity. On first open the INSERT OR IGNORE stamps
    // version=1; on subsequent opens it is a no-op.
    db_->exec("CREATE TABLE IF NOT EXISTS schema_version ("
              "id INTEGER PRIMARY KEY CHECK (id = 1), "
              "version INTEGER NOT NULL)");

    int current = 0;
    {
        SQLite::Statement q(*db_, "SELECT version FROM schema_version WHERE id = 1");
        if (q.executeStep()) {
            current = q.getColumn(0).getInt();
        }
    }

    if (current == 0) {
        // First open against a db with no recorded version. Stamp the shipped
        // version directly — the agent_proposals + agent_poll_cursor tables
        // are already created above, so an unversioned db is functionally
        // identical to a version-1 db and the migration body is empty.
        SQLite::Statement ins(*db_, "INSERT INTO schema_version (id, version) VALUES (1, ?)");
        ins.bind(1, kCurrentSchemaVersion);
        ins.exec();
        LOG_INFO("AgentProposalStore: stamped schema_version=%d (first open)", kCurrentSchemaVersion);
        return;
    }

    if (current == kCurrentSchemaVersion) {
        // Hot path: re-opening an already-versioned db. No-op.
        return;
    }

    if (current > kCurrentSchemaVersion) {
        // A db written by a future build. Refuse to downgrade silently; the
        // caller's std::runtime_error path surfaces this as an open failure.
        throw std::runtime_error(std::string("AgentProposalStore: db schema_version=") + std::to_string(current) +
                                 " is newer than build version=" + std::to_string(kCurrentSchemaVersion));
    }

    // current < kCurrentSchemaVersion: apply migrations in numeric order.
    // Each body must be idempotent (re-runnable without side-effects beyond
    // the first run) so partial-apply recovery is safe.
    while (current < kCurrentSchemaVersion) {
        if (current == 1) {
            // v1 -> v2: add agent_pr_watch table + agent_proposals.handoff_status
            // column. The CREATE TABLE in the ctor body has already executed
            // (it sits above this call) so on a fresh v0 -> v2 jump the table
            // exists at this point regardless; for a v1 -> v2 in-place upgrade
            // the CREATE TABLE IF NOT EXISTS path is a no-op too (the ctor
            // ran it before EnsureSchemaVersion). The only real work the
            // migration must do is the ADD COLUMN — and only if the column
            // is not already there (a v1 db will not have it; a partially-
            // applied v2 db might).
            if (!SqliteTableHasColumn(*db_, "agent_proposals", "handoff_status")) {
                db_->exec("ALTER TABLE agent_proposals ADD COLUMN handoff_status TEXT NOT NULL DEFAULT ''");
            }
            LOG_INFO("AgentProposalStore: migrated schema v1 -> v2 (agent_pr_watch + handoff_status)");
        }
        ++current;
    }

    SQLite::Statement up(*db_, "UPDATE schema_version SET version = ? WHERE id = 1");
    up.bind(1, kCurrentSchemaVersion);
    up.exec();
    LOG_INFO("AgentProposalStore: migrated schema_version -> %d", kCurrentSchemaVersion);
}

bool AgentProposalStore::GetSchemaVersion(int& outVersion, std::string& outError) const {
    outError.clear();
    outVersion = 0;
    try {
        SQLite::Statement q(*db_, "SELECT version FROM schema_version WHERE id = 1");
        if (!q.executeStep()) {
            outError = "schema_version row missing";
            return false;
        }
        outVersion = q.getColumn(0).getInt();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::GetSchemaVersion: ") + ex.what();
        return false;
    }
}

AgentProposalStore::~AgentProposalStore() = default;

bool AgentProposalStore::Insert(AgentProposal& proposal, std::string& outError) {
    outError.clear();
    try {
        const int64_t now = NowEpochSec();
        proposal.createdAtSec = now;
        proposal.lastUpdatedAtSec = now;
        proposal.state = AgentProposalState::Pending;

        SQLite::Statement stmt(*db_, "INSERT INTO agent_proposals (source_tracker, issue_key, action, rationale, "
                                     "payload_json, state, created_at_sec, last_updated_at_sec, apply_error) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        stmt.bind(1, proposal.sourceTracker);
        stmt.bind(2, proposal.issueKey);
        stmt.bind(3, AgenticInferenceClientPure::ActionToString(proposal.action));
        stmt.bind(4, proposal.rationale);
        const std::string payloadDump = proposal.payload.is_null() ? std::string("{}") : proposal.payload.dump();
        stmt.bind(5, payloadDump);
        stmt.bind(6, AgentProposalStateToString(proposal.state));
        stmt.bind(7, static_cast<long long>(proposal.createdAtSec));
        stmt.bind(8, static_cast<long long>(proposal.lastUpdatedAtSec));
        stmt.bind(9, proposal.applyError);
        stmt.exec();
        proposal.id = db_->getLastInsertRowid();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::Insert: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::InsertMany(std::vector<AgentProposal>& proposals, std::string& outError) {
    outError.clear();
    if (proposals.empty()) {
        return true;
    }
    try {
        // Bundle B CR#230:107 — atomic per-issue persistence. The N drafts the
        // LLM returned for one issue commit together; an insert failure on row
        // 3 of 5 rolls back rows 1-2, never leaving the user with a partial
        // proposal set for that issue. RAII rollback on exit means we only
        // commit explicitly at the end of the loop.
        SQLite::Transaction transaction(*db_);

        const int64_t now = NowEpochSec();
        SQLite::Statement stmt(*db_, "INSERT INTO agent_proposals (source_tracker, issue_key, action, rationale, "
                                     "payload_json, state, created_at_sec, last_updated_at_sec, apply_error) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        for (auto& p : proposals) {
            p.createdAtSec = now;
            p.lastUpdatedAtSec = now;
            p.state = AgentProposalState::Pending;

            stmt.reset();
            stmt.clearBindings();
            stmt.bind(1, p.sourceTracker);
            stmt.bind(2, p.issueKey);
            stmt.bind(3, AgenticInferenceClientPure::ActionToString(p.action));
            stmt.bind(4, p.rationale);
            const std::string payloadDump = p.payload.is_null() ? std::string("{}") : p.payload.dump();
            stmt.bind(5, payloadDump);
            stmt.bind(6, AgentProposalStateToString(p.state));
            stmt.bind(7, static_cast<long long>(p.createdAtSec));
            stmt.bind(8, static_cast<long long>(p.lastUpdatedAtSec));
            stmt.bind(9, p.applyError);
            stmt.exec();
            p.id = db_->getLastInsertRowid();
        }

        transaction.commit();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::InsertMany: ") + ex.what();
        // Reset stamped ids on the input vector — the rollback discarded the
        // rowids so the caller must not surface them as if they had landed.
        for (auto& p : proposals) {
            p.id = 0;
        }
        return false;
    }
}

bool AgentProposalStore::Transition(int64_t id, AgentProposalState newState, const std::string& applyError,
                                    std::string& outError) {
    outError.clear();
    // H9 — captured for the post-commit OnApprovedFn fire. The action enum
    // is read from the row inside the transaction so the callback sees the
    // exact action that was just approved; if the row vanished between
    // SELECT and the commit we never fire (the caller already sees an error).
    AgenticInferenceClientPure::ProposedAction approvedAction =
        AgenticInferenceClientPure::ProposedAction::Unknown;
    bool fireApprovedCallback = false;
    try {
        // Bundle B CR#229:177 — the SELECT-then-UPDATE sequence must be atomic.
        // Without the surrounding transaction, two concurrent transitions (UI
        // Approve racing the auto-poll worker) can both observe state=Pending,
        // both decide their transition is legal, and the second one silently
        // overwrites the first. SQLite::Transaction issues `BEGIN`, which on
        // a busy db waits up to the configured busy-timeout for the write
        // lock — the second writer then re-reads the post-commit state and
        // its transition validation catches the conflict.
        SQLite::Transaction transaction(*db_);

        AgentProposalState current;
        {
            SQLite::Statement q(*db_, "SELECT state, action FROM agent_proposals WHERE id=?");
            q.bind(1, static_cast<long long>(id));
            if (!q.executeStep()) {
                outError = "row not found";
                return false;
            }
            const std::string stateStr = q.getColumn(0).getText();
            if (!ParseAgentProposalState(stateStr, current)) {
                outError = std::string("unknown state literal in db: ") + stateStr;
                return false;
            }
            // H9 — capture the action under the same SELECT so the post-commit
            // callback sees the value that lived alongside the just-approved
            // row. A schema-drift action literal warns but doesn't block the
            // transition; the callback simply won't fire (Unknown is filtered
            // by the receiver in AppController.cpp).
            const std::string actionStr = q.getColumn(1).getText();
            if (!ParseAgenticAction(actionStr, approvedAction)) {
                LOG_WARN("AgentProposalStore::Transition: unknown action literal '%s' (row id=%lld) — "
                         "OnProposalApproved will not fire",
                         actionStr.c_str(), static_cast<long long>(id));
                approvedAction = AgenticInferenceClientPure::ProposedAction::Unknown;
            }
        }
        if (!IsLegalTransition(current, newState)) {
            outError = std::string("invalid transition: ") + AgentProposalStateToString(current) + " -> " +
                       AgentProposalStateToString(newState);
            return false;
        }
        const int64_t now = NowEpochSec();
        const std::string err = (newState == AgentProposalState::Failed) ? applyError : std::string();
        SQLite::Statement u(*db_, "UPDATE agent_proposals SET state=?, last_updated_at_sec=?, "
                                  "apply_error=? WHERE id=?");
        u.bind(1, AgentProposalStateToString(newState));
        u.bind(2, static_cast<long long>(now));
        u.bind(3, err);
        u.bind(4, static_cast<long long>(id));
        const int changed = u.exec();
        if (changed == 0) {
            outError = "row vanished between SELECT and UPDATE";
            return false;
        }
        transaction.commit();
        // H9 — flag the Pending->Approved edge for the post-commit callback
        // fire below. Other edges (Pending->Rejected, Approved->Applied, etc)
        // never trigger the handoff path. Fire OUTSIDE the try/commit scope
        // so a receiver-side exception cannot roll back the SQLite commit.
        if (current == AgentProposalState::Pending && newState == AgentProposalState::Approved &&
            approvedAction != AgenticInferenceClientPure::ProposedAction::Unknown) {
            fireApprovedCallback = true;
        }
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::Transition: ") + ex.what();
        return false;
    }

    // H9 — invoke the cross-flow callback AFTER the commit succeeds + the
    // try/catch closes. Defense in depth: the receiver in AppController also
    // re-checks `action == ImplementIssue` AND the auto-start cfg before
    // dispatching a handoff, so a future store consumer wiring a non-handoff
    // callback won't accidentally see an ImplementIssue row punted at a
    // non-handoff path. We swallow any callback-side exception with LOG_WARN
    // so a poorly-behaved receiver cannot turn a successful SQLite commit
    // into a Transition-returns-false from the caller's perspective.
    if (fireApprovedCallback && onApproved_) {
        try {
            onApproved_(id, approvedAction);
        } catch (const std::exception& ex) {
            LOG_WARN("AgentProposalStore::Transition: OnProposalApproved callback threw (id=%lld): %s",
                     static_cast<long long>(id), ex.what());
        } catch (...) {
            LOG_WARN("AgentProposalStore::Transition: OnProposalApproved callback threw non-std exception "
                     "(id=%lld)",
                     static_cast<long long>(id));
        }
    }
    return true;
}

void AgentProposalStore::SetOnProposalApproved(OnApprovedFn cb) { onApproved_ = std::move(cb); }

bool AgentProposalStore::Query(const Filter& f, std::vector<AgentProposal>& out, std::string& outError) const {
    out.clear();
    outError.clear();
    try {
        // Bundle C — cap Filter::states defensively. The vector is concatenated
        // into a SQL `IN (?, ?, ...)` placeholder list; an unbounded caller-
        // supplied vector would compile a correspondingly large statement.
        // Truncate (rather than reject) so the typical 1-5 entry use-case
        // continues to work; a caller exceeding the cap sees a one-shot warning
        // and the first kMaxFilterStates entries are used.
        std::vector<AgentProposalState> states = f.states;
        if (states.size() > kMaxFilterStates) {
            LOG_WARN("AgentProposalStore::Query: Filter::states capped from %zu to %zu",
                     states.size(), static_cast<std::size_t>(kMaxFilterStates));
            states.resize(kMaxFilterStates);
        }

        std::string sql = "SELECT id, source_tracker, issue_key, action, rationale, payload_json, "
                          "state, created_at_sec, last_updated_at_sec, apply_error FROM agent_proposals";
        std::vector<std::string> whereClauses;
        if (!states.empty()) {
            std::string inList;
            for (size_t i = 0; i < states.size(); ++i) {
                if (i > 0)
                    inList += ", ";
                inList += "?";
            }
            whereClauses.push_back("state IN (" + inList + ")");
        }
        if (!f.sourceTracker.empty()) {
            whereClauses.push_back("source_tracker = ?");
        }
        if (!f.issueKey.empty()) {
            whereClauses.push_back("issue_key = ?");
        }
        for (size_t i = 0; i < whereClauses.size(); ++i) {
            sql += (i == 0) ? " WHERE " : " AND ";
            sql += whereClauses[i];
        }
        sql += " ORDER BY id ASC";
        if (f.limit > 0) {
            sql += " LIMIT ?";
        }

        SQLite::Statement q(*db_, sql);
        int idx = 1;
        for (const auto s : states) {
            q.bind(idx++, AgentProposalStateToString(s));
        }
        if (!f.sourceTracker.empty()) {
            q.bind(idx++, f.sourceTracker);
        }
        if (!f.issueKey.empty()) {
            q.bind(idx++, f.issueKey);
        }
        if (f.limit > 0) {
            q.bind(idx++, f.limit);
        }

        while (q.executeStep()) {
            AgentProposal p;
            std::string rowErr;
            if (!RowToProposal(q, p, rowErr)) {
                // Bundle B CR#229:318 — surface schema-drift loudly. A row
                // with an unknown action / state literal indicates a db
                // written by a future build; the safe behaviour is to fail
                // the whole query so the caller doesn't silently re-action
                // misclassified rows. The partial `out` is cleared to keep
                // the result-set semantics atomic (all-or-nothing).
                out.clear();
                outError = "AgentProposalStore::Query: " + rowErr;
                return false;
            }
            out.push_back(std::move(p));
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::Query: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::Find(int64_t id, AgentProposal& out, std::string& outError) const {
    outError.clear();
    try {
        SQLite::Statement q(*db_, "SELECT id, source_tracker, issue_key, action, rationale, payload_json, "
                                  "state, created_at_sec, last_updated_at_sec, apply_error FROM agent_proposals "
                                  "WHERE id=?");
        q.bind(1, static_cast<long long>(id));
        if (!q.executeStep()) {
            outError = "row not found";
            return false;
        }
        std::string rowErr;
        if (!RowToProposal(q, out, rowErr)) {
            outError = "AgentProposalStore::Find: " + rowErr;
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::Find: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::GetPollCursor(const std::string& sourceTracker, const std::string& repoKey,
                                       int64_t& outLastSeenUpdatedAt, std::string& outError) const {
    outError.clear();
    outLastSeenUpdatedAt = 0;
    try {
        SQLite::Statement q(*db_, "SELECT last_seen_updated_at FROM agent_poll_cursor "
                                  "WHERE source_tracker=? AND repo_key=?");
        q.bind(1, sourceTracker);
        q.bind(2, repoKey);
        if (q.executeStep()) {
            outLastSeenUpdatedAt = q.getColumn(0).getInt64();
        }
        // First-call semantics: row absent -> outLastSeenUpdatedAt stays 0; still ok=true.
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::GetPollCursor: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetPrWatch(const PrWatchRow& row, std::string& outError) {
    outError.clear();
    try {
        SQLite::Statement stmt(*db_, "INSERT OR REPLACE INTO agent_pr_watch "
                                     "(proposal_id, pr_url, last_seen_comment_id_str, iteration_count, "
                                     "last_polled_at_sec) VALUES (?, ?, ?, ?, ?)");
        stmt.bind(1, static_cast<long long>(row.proposalId));
        stmt.bind(2, row.prUrl);
        stmt.bind(3, row.lastSeenCommentIdStr);
        stmt.bind(4, row.iterationCount);
        stmt.bind(5, static_cast<long long>(row.lastPolledAtSec));
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetPrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::GetPrWatch(std::int64_t proposalId, PrWatchRow& outRow, std::string& outError) const {
    outError.clear();
    outRow = PrWatchRow();
    try {
        SQLite::Statement q(*db_, "SELECT proposal_id, pr_url, last_seen_comment_id_str, iteration_count, "
                                  "last_polled_at_sec FROM agent_pr_watch WHERE proposal_id=?");
        q.bind(1, static_cast<long long>(proposalId));
        if (q.executeStep()) {
            outRow.proposalId = q.getColumn(0).getInt64();
            outRow.prUrl = q.getColumn(1).getText();
            outRow.lastSeenCommentIdStr = q.getColumn(2).getText();
            outRow.iterationCount = q.getColumn(3).getInt();
            outRow.lastPolledAtSec = q.getColumn(4).getInt64();
        }
        // Row absent -> defaulted outRow + ok=true (first-poll semantics).
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::GetPrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::DeletePrWatch(std::int64_t proposalId, std::string& outError) {
    outError.clear();
    try {
        SQLite::Statement stmt(*db_, "DELETE FROM agent_pr_watch WHERE proposal_id=?");
        stmt.bind(1, static_cast<long long>(proposalId));
        stmt.exec();
        // Missing row is not an error — DELETE on a non-existent row returns 0
        // rows changed, which is the documented contract.
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::DeletePrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetOpenPrWatch(const OpenPrWatchRow& row, std::string& outError) {
    outError.clear();
    try {
        // ON CONFLICT … DO UPDATE preserves cursor fields on re-upsert (the
        // registrar passes default-zero cursors on every tick; INSERT OR
        // REPLACE would zero them and we'd lose the watcher's place every
        // poll). Only the head-tracking fields (pr_number, head_ref_name,
        // head_sha) rebind on conflict — a force-push shifts head_sha, but
        // last_seen_comment_id_str / last_seen_check_run_id / iteration_count
        // / last_polled_at_sec stay put. Requires SQLite 3.24+; the Smatchet
        // FetchContent build ships a newer SQLiteCpp / SQLite.
        SQLite::Statement stmt(*db_, "INSERT INTO agent_open_pr_watch "
                                     "(pr_url, pr_number, head_ref_name, head_sha, last_seen_comment_id_str, "
                                     " last_seen_check_run_id, iteration_count, last_polled_at_sec) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                                     "ON CONFLICT (pr_url) DO UPDATE SET "
                                     "  pr_number = excluded.pr_number, "
                                     "  head_ref_name = excluded.head_ref_name, "
                                     "  head_sha = excluded.head_sha");
        stmt.bind(1, row.prUrl);
        stmt.bind(2, row.prNumber);
        stmt.bind(3, row.headRefName);
        stmt.bind(4, row.headSha);
        stmt.bind(5, row.lastSeenCommentIdStr);
        stmt.bind(6, static_cast<long long>(row.lastSeenCheckRunId));
        stmt.bind(7, row.iterationCount);
        stmt.bind(8, static_cast<long long>(row.lastPolledAtSec));
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetOpenPrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::GetOpenPrWatch(const std::string& prUrl, OpenPrWatchRow& outRow,
                                        std::string& outError) const {
    outError.clear();
    outRow = OpenPrWatchRow();
    try {
        SQLite::Statement q(*db_,
                            "SELECT pr_url, pr_number, head_ref_name, head_sha, last_seen_comment_id_str, "
                            "       last_seen_check_run_id, iteration_count, last_polled_at_sec "
                            "FROM agent_open_pr_watch WHERE pr_url=?");
        q.bind(1, prUrl);
        if (q.executeStep()) {
            outRow.prUrl = q.getColumn(0).getText();
            outRow.prNumber = q.getColumn(1).getInt();
            outRow.headRefName = q.getColumn(2).getText();
            outRow.headSha = q.getColumn(3).getText();
            outRow.lastSeenCommentIdStr = q.getColumn(4).getText();
            outRow.lastSeenCheckRunId = q.getColumn(5).getInt64();
            outRow.iterationCount = q.getColumn(6).getInt();
            outRow.lastPolledAtSec = q.getColumn(7).getInt64();
        }
        // Row absent -> defaulted outRow + ok=true (first-poll semantics).
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::GetOpenPrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::ListOpenPrWatch(std::vector<OpenPrWatchRow>& outRows, std::string& outError) const {
    outError.clear();
    outRows.clear();
    try {
        SQLite::Statement q(*db_,
                            "SELECT pr_url, pr_number, head_ref_name, head_sha, last_seen_comment_id_str, "
                            "       last_seen_check_run_id, iteration_count, last_polled_at_sec "
                            "FROM agent_open_pr_watch ORDER BY pr_url ASC");
        while (q.executeStep()) {
            OpenPrWatchRow r;
            r.prUrl = q.getColumn(0).getText();
            r.prNumber = q.getColumn(1).getInt();
            r.headRefName = q.getColumn(2).getText();
            r.headSha = q.getColumn(3).getText();
            r.lastSeenCommentIdStr = q.getColumn(4).getText();
            r.lastSeenCheckRunId = q.getColumn(5).getInt64();
            r.iterationCount = q.getColumn(6).getInt();
            r.lastPolledAtSec = q.getColumn(7).getInt64();
            outRows.push_back(std::move(r));
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::ListOpenPrWatch: ") + ex.what();
        outRows.clear();
        return false;
    }
}

bool AgentProposalStore::DeleteOpenPrWatch(const std::string& prUrl, std::string& outError) {
    outError.clear();
    try {
        SQLite::Statement stmt(*db_, "DELETE FROM agent_open_pr_watch WHERE pr_url=?");
        stmt.bind(1, prUrl);
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::DeleteOpenPrWatch: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetOpenPrCommentCursor(const std::string& prUrl, const std::string& lastSeenCommentIdStr,
                                                std::string& outError) {
    outError.clear();
    try {
        // Idempotent — UPDATE on a missing row returns 0 rows changed and is
        // not an error. The phase-4 watcher relies on this when an open-PR
        // row was deleted by the registrar mid-tick (PR closed upstream).
        SQLite::Statement stmt(*db_, "UPDATE agent_open_pr_watch SET last_seen_comment_id_str=?, "
                                     "last_polled_at_sec=? WHERE pr_url=?");
        stmt.bind(1, lastSeenCommentIdStr);
        stmt.bind(2, static_cast<long long>(NowEpochSec()));
        stmt.bind(3, prUrl);
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetOpenPrCommentCursor: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetOpenPrCheckRunCursor(const std::string& prUrl, std::int64_t lastSeenCheckRunId,
                                                  std::string& outError) {
    outError.clear();
    try {
        // Idempotent — UPDATE on a missing row returns 0 rows changed and is
        // not an error. Mirrors SetOpenPrCommentCursor; bumps the same
        // `last_polled_at_sec` field so a check-run-only tick is observable
        // as recent activity.
        SQLite::Statement stmt(*db_, "UPDATE agent_open_pr_watch SET last_seen_check_run_id=?, "
                                     "last_polled_at_sec=? WHERE pr_url=?");
        stmt.bind(1, static_cast<long long>(lastSeenCheckRunId));
        stmt.bind(2, static_cast<long long>(NowEpochSec()));
        stmt.bind(3, prUrl);
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetOpenPrCheckRunCursor: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::IncrementOpenPrIterationCount(const std::string& prUrl, int& outNewCount,
                                                       std::string& outError) {
    outError.clear();
    outNewCount = 0;
    try {
        // SELECT-then-UPDATE inside an explicit transaction. The bundled
        // SQLite supports RETURNING (3.35+) but doing the read+write
        // explicitly keeps the SQL portable across the wider SQLiteCpp /
        // amalgamation range used in archived state-replay tooling. The
        // BEGIN locks out a concurrent writer so the read sees a coherent
        // snapshot.
        SQLite::Transaction transaction(*db_);
        int current = -1;
        {
            SQLite::Statement q(*db_, "SELECT iteration_count FROM agent_open_pr_watch WHERE pr_url=?");
            q.bind(1, prUrl);
            if (!q.executeStep()) {
                outError = "row not found";
                return false;
            }
            current = q.getColumn(0).getInt();
        }
        const int next = current + 1;
        {
            SQLite::Statement u(*db_, "UPDATE agent_open_pr_watch SET iteration_count=?, last_polled_at_sec=? "
                                      "WHERE pr_url=?");
            u.bind(1, next);
            u.bind(2, static_cast<long long>(NowEpochSec()));
            u.bind(3, prUrl);
            const int changed = u.exec();
            if (changed == 0) {
                outError = "row vanished between SELECT and UPDATE";
                return false;
            }
        }
        transaction.commit();
        outNewCount = next;
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::IncrementOpenPrIterationCount: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetHandoffStatus(std::int64_t proposalId, const std::string& status, std::string& outError) {
    outError.clear();
    try {
        SQLite::Statement stmt(*db_, "UPDATE agent_proposals SET handoff_status=? WHERE id=?");
        stmt.bind(1, status);
        stmt.bind(2, static_cast<long long>(proposalId));
        const int changed = stmt.exec();
        if (changed == 0) {
            outError = "row not found";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetHandoffStatus: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::GetHandoffStatus(std::int64_t proposalId, std::string& outStatus,
                                          std::string& outError) const {
    outError.clear();
    outStatus.clear();
    try {
        SQLite::Statement q(*db_, "SELECT handoff_status FROM agent_proposals WHERE id=?");
        q.bind(1, static_cast<long long>(proposalId));
        if (!q.executeStep()) {
            outError = "row not found";
            return false;
        }
        outStatus = q.getColumn(0).getText();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::GetHandoffStatus: ") + ex.what();
        return false;
    }
}

bool AgentProposalStore::SetPollCursor(const std::string& sourceTracker, const std::string& repoKey,
                                       int64_t lastSeenUpdatedAt, std::string& outError) {
    outError.clear();
    try {
        // INSERT OR REPLACE atomically overwrites any prior row for the (tracker, repo) tuple.
        SQLite::Statement stmt(*db_, "INSERT OR REPLACE INTO agent_poll_cursor "
                                     "(source_tracker, repo_key, last_seen_updated_at) VALUES (?, ?, ?)");
        stmt.bind(1, sourceTracker);
        stmt.bind(2, repoKey);
        stmt.bind(3, static_cast<long long>(lastSeenUpdatedAt));
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::SetPollCursor: ") + ex.what();
        return false;
    }
}

// String helpers — defined in this TU so they share the AGENTIC source-list gate
// with the SQLite store. Anyone who needs them in the OFF build can lift them
// into a pure header later; no current consumer does.

const char* AgentProposalStateToString(AgentProposalState s) {
    switch (s) {
    case AgentProposalState::Pending:
        return "Pending";
    case AgentProposalState::Approved:
        return "Approved";
    case AgentProposalState::Rejected:
        return "Rejected";
    case AgentProposalState::Applied:
        return "Applied";
    case AgentProposalState::Failed:
        return "Failed";
    }
    return "Pending";
}

bool ParseAgentProposalState(const std::string& s, AgentProposalState& out) {
    if (s == "Pending") {
        out = AgentProposalState::Pending;
        return true;
    }
    if (s == "Approved") {
        out = AgentProposalState::Approved;
        return true;
    }
    if (s == "Rejected") {
        out = AgentProposalState::Rejected;
        return true;
    }
    if (s == "Applied") {
        out = AgentProposalState::Applied;
        return true;
    }
    if (s == "Failed") {
        out = AgentProposalState::Failed;
        return true;
    }
    return false;
}

bool ParseAgenticAction(const std::string& s, AgenticInferenceClientPure::ProposedAction& out) {
    using A = AgenticInferenceClientPure::ProposedAction;
    if (s == "CommentAdd") {
        out = A::CommentAdd;
        return true;
    }
    if (s == "LabelAdd") {
        out = A::LabelAdd;
        return true;
    }
    if (s == "LabelRemove") {
        out = A::LabelRemove;
        return true;
    }
    if (s == "AssigneeSet") {
        out = A::AssigneeSet;
        return true;
    }
    if (s == "StateTransition") {
        out = A::StateTransition;
        return true;
    }
    if (s == "DerivedTicketCreate") {
        out = A::DerivedTicketCreate;
        return true;
    }
    if (s == "ImplementIssue") {
        out = A::ImplementIssue;
        return true;
    }
    if (s == "Unknown") {
        out = A::Unknown;
        return true;
    }
    return false;
}
