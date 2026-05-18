#include "AgenticProposalAuditPure.h"

namespace smatchet {
namespace agentic {
namespace pure {

const char* ProposalActionLabel(AgentProposalState from, AgentProposalState to) {
    // The two user-driven transitions T6 wires today. ProposalActionLabel is a
    // sealed enum-pair lookup; new transitions add explicit branches rather
    // than auto-deriving from enum names (the audit-log consumer's filter
    // grammar treats these as stable identifiers).
    if (from == AgentProposalState::Pending && to == AgentProposalState::Approved) {
        return "ProposalApprove";
    }
    if (from == AgentProposalState::Pending && to == AgentProposalState::Rejected) {
        return "ProposalReject";
    }
    return "ProposalTransition";
}

namespace {

BackendAuditTrail::AuditEvent MakeEventCommon(std::int64_t proposalId, const std::string& issueKey,
                                              AgentProposalState fromState, AgentProposalState toState,
                                              const std::string& operationId) {
    BackendAuditTrail::AuditEvent ev;
    ev.Action = ProposalActionLabel(fromState, toState);
    ev.Source = kAgenticAuditSource;
    ev.IssueKey = issueKey;
    ev.OperationId = operationId;
    ev.Data = nlohmann::json::object();
    ev.Data["id"] = proposalId;
    ev.Data["fromState"] = AgentProposalStateToString(fromState);
    ev.Data["toState"] = AgentProposalStateToString(toState);
    return ev;
}

} // namespace

BackendAuditTrail::AuditEvent MakeBeginEvent(std::int64_t proposalId, const std::string& issueKey,
                                             AgentProposalState fromState, AgentProposalState toState,
                                             const std::string& operationId) {
    BackendAuditTrail::AuditEvent ev = MakeEventCommon(proposalId, issueKey, fromState, toState, operationId);
    ev.Phase = "begin";
    ev.Success = false;
    return ev;
}

BackendAuditTrail::AuditEvent MakeResultEvent(std::int64_t proposalId, const std::string& issueKey,
                                              AgentProposalState fromState, AgentProposalState toState,
                                              const std::string& operationId, bool success,
                                              const std::string& error) {
    BackendAuditTrail::AuditEvent ev = MakeEventCommon(proposalId, issueKey, fromState, toState, operationId);
    ev.Phase = "result";
    ev.Success = success;
    ev.Error = error;
    return ev;
}

} // namespace pure
} // namespace agentic
} // namespace smatchet
