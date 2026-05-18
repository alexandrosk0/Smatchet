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

void RowToProposal(SQLite::Statement& q, AgentProposal& out) {
    out.id = q.getColumn(0).getInt64();
    out.sourceTracker = q.getColumn(1).getText();
    out.issueKey = q.getColumn(2).getText();
    const std::string actionStr = q.getColumn(3).getText();
    if (!ParseAgenticAction(actionStr, out.action)) {
        out.action = AgenticInferenceClientPure::ProposedAction::Unknown;
        LOG_WARN("AgentProposalStore: unknown action literal in db: %s", actionStr.c_str());
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
        out.state = AgentProposalState::Pending;
        LOG_WARN("AgentProposalStore: unknown state literal in db: %s", stateStr.c_str());
    }
    out.createdAtSec = q.getColumn(7).getInt64();
    out.lastUpdatedAtSec = q.getColumn(8).getInt64();
    out.applyError = q.getColumn(9).getText();
}

} // namespace

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

    LOG_INFO("AgentProposalStore: schema ready");
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

bool AgentProposalStore::Transition(int64_t id, AgentProposalState newState, const std::string& applyError,
                                    std::string& outError) {
    outError.clear();
    try {
        // Load current state to validate the transition. Done in the same TU as the UPDATE
        // so we don't double-encode the state machine; a single SELECT keeps the round-trip
        // honest and avoids racing with another writer (SQLite serialises writes).
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
        std::string sql = "SELECT id, source_tracker, issue_key, action, rationale, payload_json, "
                          "state, created_at_sec, last_updated_at_sec, apply_error FROM agent_proposals";
        std::vector<std::string> whereClauses;
        if (!f.states.empty()) {
            std::string inList;
            for (size_t i = 0; i < f.states.size(); ++i) {
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
        for (const auto s : f.states) {
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
            RowToProposal(q, p);
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
        RowToProposal(q, out);
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
