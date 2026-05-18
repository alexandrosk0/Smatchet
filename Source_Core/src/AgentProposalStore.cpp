#include "AgentProposalStore.h"

#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

int64_t NowEpochSec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
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

    // Records the shipped schema version. AGENTS.md § Schema-version bumps
    // requires exactly one shipped increment per feature increment; the agentic
    // tables were additive across T4-T8 (new tables only) and only formally
    // land their first shipped version here.
    EnsureSchemaVersion();

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

    // current < kCurrentSchemaVersion: future migrations land here. Loop one
    // step at a time so each migration body sees the exact prior state. For
    // now (kCurrentSchemaVersion = 1) this branch is unreachable; kept so
    // adding version 2 only requires writing one `if (current == 1) { … }`
    // block plus bumping the constant.
    while (current < kCurrentSchemaVersion) {
        // Placeholder: per-version migration bodies are inserted here as the
        // schema grows. Each body must be idempotent (re-runnable without
        // side-effects beyond the first run) so partial-apply recovery is
        // safe.
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
            SQLite::Statement q(*db_, "SELECT state FROM agent_proposals WHERE id=?");
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
        return true;
    } catch (const std::exception& ex) {
        outError = std::string("AgentProposalStore::Transition: ") + ex.what();
        return false;
    }
}

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
