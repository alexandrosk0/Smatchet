// AgenticProposalAuditPure tests — cover the audit-trail entry shape emitted
// by SmatchetAgentProposalsUi on Approve / Reject. The test rig captures the
// invariant that a worker-thread transition writes a Begin + Result pair with
// the canonical action label, source, and (fromState, toState) payload —
// regression of which would silently re-introduce SH2 (local SQLite edit
// indistinguishable from a real backend write).

#include "AgenticProposalAuditPure.h"

#include "AgentProposal.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::agentic::pure::kAgenticAuditSource;
using smatchet::agentic::pure::MakeBeginEvent;
using smatchet::agentic::pure::MakeResultEvent;
using smatchet::agentic::pure::ProposalActionLabel;

TEST_CASE("ProposalActionLabel — Pending -> Approved maps to ProposalApprove") {
    CHECK(std::string(ProposalActionLabel(AgentProposalState::Pending, AgentProposalState::Approved)) ==
          "ProposalApprove");
}

TEST_CASE("ProposalActionLabel — Pending -> Rejected maps to ProposalReject") {
    CHECK(std::string(ProposalActionLabel(AgentProposalState::Pending, AgentProposalState::Rejected)) ==
          "ProposalReject");
}

TEST_CASE("ProposalActionLabel — unmapped transitions fall back to ProposalTransition") {
    // Future-proofing: new state pairs that are wired before this helper is
    // updated should still surface a well-formed audit entry, not a crash.
    CHECK(std::string(ProposalActionLabel(AgentProposalState::Approved, AgentProposalState::Applied)) ==
          "ProposalTransition");
    CHECK(std::string(ProposalActionLabel(AgentProposalState::Approved, AgentProposalState::Failed)) ==
          "ProposalTransition");
}

TEST_CASE("MakeBeginEvent — populates action/source/phase/data shape") {
    const auto ev = MakeBeginEvent(/*proposalId*/ 42, /*issueKey*/ "alex/repo#7",
                                   AgentProposalState::Pending, AgentProposalState::Approved,
                                   /*operationId*/ "agentic-proposals-deadbeef");
    CHECK(ev.Action == "ProposalApprove");
    CHECK(ev.Source == kAgenticAuditSource);
    CHECK(ev.IssueKey == "alex/repo#7");
    CHECK(ev.OperationId == "agentic-proposals-deadbeef");
    CHECK(ev.Phase == "begin");
    CHECK_FALSE(ev.Success);
    CHECK(ev.Error.empty());
    REQUIRE(ev.Data.is_object());
    REQUIRE(ev.Data.contains("id"));
    REQUIRE(ev.Data.contains("fromState"));
    REQUIRE(ev.Data.contains("toState"));
    CHECK(ev.Data["id"].get<std::int64_t>() == 42);
    CHECK(ev.Data["fromState"].get<std::string>() == std::string(AgentProposalStateToString(AgentProposalState::Pending)));
    CHECK(ev.Data["toState"].get<std::string>() ==
          std::string(AgentProposalStateToString(AgentProposalState::Approved)));
}

TEST_CASE("MakeResultEvent — success path carries empty error + success=true") {
    const auto ev = MakeResultEvent(/*proposalId*/ 17, /*issueKey*/ "alex/repo#3",
                                    AgentProposalState::Pending, AgentProposalState::Rejected,
                                    /*operationId*/ "agentic-proposals-1234",
                                    /*success*/ true, /*error*/ "");
    CHECK(ev.Action == "ProposalReject");
    CHECK(ev.Phase == "result");
    CHECK(ev.Success);
    CHECK(ev.Error.empty());
    CHECK(ev.Data["fromState"].get<std::string>() ==
          std::string(AgentProposalStateToString(AgentProposalState::Pending)));
    CHECK(ev.Data["toState"].get<std::string>() ==
          std::string(AgentProposalStateToString(AgentProposalState::Rejected)));
}

TEST_CASE("MakeResultEvent — failure path carries error text + success=false") {
    const auto ev = MakeResultEvent(/*proposalId*/ 99, /*issueKey*/ "alex/repo#9",
                                    AgentProposalState::Pending, AgentProposalState::Approved,
                                    /*operationId*/ "agentic-proposals-cafe",
                                    /*success*/ false, /*error*/ "DB locked");
    CHECK(ev.Phase == "result");
    CHECK_FALSE(ev.Success);
    CHECK(ev.Error == "DB locked");
    CHECK(ev.Data["id"].get<std::int64_t>() == 99);
}
