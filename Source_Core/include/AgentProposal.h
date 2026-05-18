#ifndef SMATCHET_AGENT_PROPOSAL_H
#define SMATCHET_AGENT_PROPOSAL_H

// AgentProposal — the persisted lifecycle record for an LLM-suggested action.
// Builds on `AgenticInferenceClientPure::ProposedAction` from T3; the inference
// client emits short-lived `ProposalDraft` objects that the orchestrator (T5)
// promotes into `AgentProposal` rows via `AgentProposalStore::Insert`.
//
// Lifecycle state machine (enforced by AgentProposalStore::Transition):
//   Pending  -> Approved | Rejected
//   Approved -> Applied  | Failed
//   Rejected, Applied, Failed are TERMINAL — re-attempts insert a NEW Pending
//   row rather than reviving a terminal one. This keeps the audit trail honest.
//
// Schema serialisation lives in AgentProposalStore.cpp; the wire shape for the
// `action` column reuses `AgenticInferenceClientPure::ActionToString` so a
// proposal can be round-tripped through SQLite without a second enum spelling.

#include "AgenticInferenceClientPure.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

enum class AgentProposalState {
    Pending,
    Approved,
    Rejected,
    Applied,
    Failed,
};

struct AgentProposal {
    // Identity. id == 0 means "unsaved"; AgentProposalStore::Insert stamps the
    // SQLite ROWID on a successful insert.
    int64_t id = 0;
    std::string sourceTracker; // "github" / "jira" / "plane"
    std::string issueKey;      // backend-stable key (e.g. "smatchet/example#42")

    // Inference output, promoted verbatim from `AgenticInferenceClientPure::ProposalDraft`.
    AgenticInferenceClientPure::ProposedAction action = AgenticInferenceClientPure::ProposedAction::Unknown;
    std::string rationale;
    nlohmann::json payload; // action-specific shape — see decision #3 of the plan.

    // Lifecycle.
    AgentProposalState state = AgentProposalState::Pending;
    int64_t createdAtSec = 0;     // unix epoch, set on first insert.
    int64_t lastUpdatedAtSec = 0; // unix epoch, bumped on every state transition.
    std::string applyError;       // populated on Failed; empty otherwise.
};

// Canonical lowercase-camel string for the enum, stable across versions. Used
// by AgentProposalStore for SQLite serialisation and by the parser helper
// below. Unknown returns "Unknown".
const char* AgentProposalStateToString(AgentProposalState s);

// Inverse of AgentProposalStateToString. Returns false + leaves `out`
// untouched when `s` is not a known literal.
bool ParseAgentProposalState(const std::string& s, AgentProposalState& out);

// Inverse of AgenticInferenceClientPure::ActionToString. Mirrors the parser
// contract above. Lives here (next to the proposal record) rather than in
// AgenticInferenceClientPure because T3 only needed one-way serialisation;
// T4 (SQLite round-trip) is the first consumer that needs the reverse.
bool ParseAgenticAction(const std::string& s, AgenticInferenceClientPure::ProposedAction& out);

#endif // SMATCHET_AGENT_PROPOSAL_H
