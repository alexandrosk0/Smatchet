#ifndef SMATCHET_AGENTIC_PROPOSAL_AUDIT_PURE_H
#define SMATCHET_AGENTIC_PROPOSAL_AUDIT_PURE_H

// Pure helper — constructs the BackendAuditTrail::AuditEvent payload that
// SmatchetAgentProposalsUi emits when the user clicks Approve / Reject. Sits
// here (rather than inline in the UI TU) so the audit-trail entry shape is
// covered by a doctest without pulling ImGui / AppController / SQLite.
//
// Pure C++14; no third-party deps beyond nlohmann/json (already in the test
// CMake target).
//
// Owner: SmatchetAgentProposalsUi (UI) — pure helpers live here.
// Test surface: tests/Source_Core/AgenticProposalAuditPure.test.cpp.

#include "AgentProposal.h"
#include "BackendAuditTrail.h"

#include <cstdint>
#include <string>

namespace smatchet {
namespace agentic {
namespace pure {

/// Audit-trail "source" constant for agentic-flow approval / rejection events.
/// Matches the kGitHubAuditSource pattern (lowercase snake) so downstream log
/// consumers can filter by Source verbatim.
constexpr const char* kAgenticAuditSource = "agentic_proposals";

/// Action label written to AuditEvent::Action for a state transition. The
/// canonical pair is "ProposalApprove" / "ProposalReject" — the from-state is
/// always Pending in T6-shipped flows, so the action name encodes the user
/// intent (not the raw enum target).
///
/// Returns a stable string literal for the two user-driven transitions and
/// "ProposalTransition" for any other (future) state pair, so the entry shape
/// stays well-formed even when a new transition is wired without a label
/// rule update.
const char* ProposalActionLabel(AgentProposalState from, AgentProposalState to);

/// Constructs the AuditEvent that should be appended at the "begin" phase
/// (before the SQLite UPDATE) for an Approve/Reject click. Stamps:
///   - Action     = ProposalActionLabel(from, to)
///   - Source     = kAgenticAuditSource
///   - IssueKey   = issueKey (as known to the proposal row)
///   - OperationId= operationId (caller-supplied — typically MakeOperationId)
///   - Phase      = "begin"
///   - Success    = false (begin phase has no outcome yet)
///   - Data       = { "id": <proposal id>, "fromState": "...", "toState": "..." }
///
/// Caller passes the result to BackendAuditTrail::AppendEvent.
BackendAuditTrail::AuditEvent MakeBeginEvent(std::int64_t proposalId, const std::string& issueKey,
                                             AgentProposalState fromState, AgentProposalState toState,
                                             const std::string& operationId);

/// Constructs the AuditEvent for the "result" phase (after the SQLite UPDATE).
/// Same action / source / issueKey / operationId as the begin event. Differs in
/// Phase ("result"), Success (the caller-supplied outcome), and Error (empty
/// on success, error text on failure).
BackendAuditTrail::AuditEvent MakeResultEvent(std::int64_t proposalId, const std::string& issueKey,
                                              AgentProposalState fromState, AgentProposalState toState,
                                              const std::string& operationId, bool success,
                                              const std::string& error);

} // namespace pure
} // namespace agentic
} // namespace smatchet

#endif
