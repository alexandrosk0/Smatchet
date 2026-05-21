// AgentTriageScenarioStep — end-to-end regression scenario for the agentic
// triage flow shipped in T0-T7.
//
// Exercises AgenticTriageController against mock IGitHubReadClient +
// IInferenceClient seams (introduced in T5) so no live HTTP traffic, no
// Ollama runtime, and no LLM credits are burned per CI run. The fixtures
// (tests/fixtures/github_issues_sample.json + ollama_chat_sample.json) are
// loaded indirectly: we use the same per-action payload shapes the schema
// validates so the in-process mocks remain wire-compatible without touching
// the filesystem from the scenario hot path.
//
// Frame-state machine:
//   OnStart            — wire mocks + controller + :memory: store
//   OnFrame[0]         — TriageBatch("github", "fake/repo")
//   OnFrame[1]         — assert N>0 Pending rows; all sourceTracker=="github"
//   OnFrame[2]         — Transition #1 Pending->Approved; re-query, assert
//   OnFrame[3]         — Transition #2 Pending->Rejected; re-query, assert
//   IsDone             — true after frame 3 OR on assertion failure
//   OnFinish           — JSON {passed, proposals_inserted, approved_id,
//                              rejected_id, state_transitions_ok, errors}
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC in the
// root CMakeLists.txt; the factory call in AppController.cpp wraps in
// `#if defined(SMATCHET_WITH_AGENTIC)` so the OFF build has no dangling
// reference to MakeAgentTriageScenario.

#include "Commands/Scenarios/IScenario.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"
#include "AgenticTriageController.h"
#include "Logger.h"
#include "UiPerfMonitor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

using ::AgenticInferenceClientPure::ProposalDraft;
using ::AgenticInferenceClientPure::ProposedAction;
using ::smatchet::agentic::AgenticTriageController;
using ::smatchet::agentic::IGitHubReadClient;
using ::smatchet::agentic::IInferenceClient;

// Mock IGitHubReadClient — canned 3 issues with 5 comments total across them.
// The shape mirrors what tests/fixtures/github_issues_sample.json records on
// disk; we encode it inline so the scenario stays hermetic (no filesystem
// reach during the per-frame hot path).
class ScenarioMockGitHub : public IGitHubReadClient {
  public:
    bool FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError) override {
        (void)outError;
        // The body content does not influence the mock inference output, but
        // varying it per-key lets a future regression assertion catch
        // "controller passed the wrong body" if/when we add such a check.
        outBody = "Mock body for " + issueKey;
        return true;
    }
    bool FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                            std::string& outError) override {
        (void)outError;
        outComments.clear();
        // 3 / 1 / 1 comment distribution across the canned issues — total 5.
        if (issueKey == "fake/repo#1") {
            TrackerIssueComment c;
            c.Author = "alice";
            c.Body = "Repros for me on macOS.";
            c.CreatedAtSec = 1778423011;
            outComments.push_back(c);
            c.Author = "bob";
            c.Body = "Duplicate of #98?";
            c.CreatedAtSec = 1778425302;
            outComments.push_back(c);
            c.Author = "smatchet-bot";
            c.Body = "<!-- smatchet-handoff --> Iteration 1 opened.";
            c.CreatedAtSec = 1778430600;
            outComments.push_back(c);
        } else if (issueKey == "fake/repo#2") {
            TrackerIssueComment c;
            c.Author = "carol";
            c.Body = "Adding repro steps.";
            c.CreatedAtSec = 1778431000;
            outComments.push_back(c);
        } else if (issueKey == "fake/repo#3") {
            TrackerIssueComment c;
            c.Author = "dave";
            c.Body = "+1, also affecting our team.";
            c.CreatedAtSec = 1778432000;
            outComments.push_back(c);
        }
        return true;
    }
    bool ListOpenIssuesForRepo(const std::string& owner, const std::string& repo, std::vector<std::string>& outKeys,
                               std::string& outError, std::int64_t /*sinceUnixSec*/ = 0) override {
        (void)outError;
        const std::string prefix = owner + "/" + repo;
        outKeys = {prefix + "#1", prefix + "#2", prefix + "#3"};
        return true;
    }
};

// Mock IInferenceClient — returns 3 canned drafts per call (CommentAdd +
// LabelAdd + ImplementIssue). Mirrors the structure of
// tests/fixtures/ollama_chat_sample.json without pulling in the file: the
// schema-validation rules live in AgenticInferenceClientPure and the doctest
// rig already covers the fixture's round-trip.
class ScenarioMockInference : public IInferenceClient {
  public:
    bool RequestProposals(const std::string& /*issueBody*/, const std::vector<TrackerIssueComment>& /*comments*/,
                          std::vector<ProposalDraft>& outDrafts, std::string& outError) override {
        (void)outError;
        outDrafts.clear();

        ProposalDraft c;
        c.action = ProposedAction::CommentAdd;
        c.rationale = "Ask reporter for repro steps before triaging.";
        c.payload = nlohmann::json{{"body", "Could you provide a minimal repro case?"}};
        outDrafts.push_back(c);

        ProposalDraft l;
        l.action = ProposedAction::LabelAdd;
        l.rationale = "Tag for the offline-sync subsystem owner.";
        l.payload = nlohmann::json{{"label", "subsystem:offline-sync"}};
        outDrafts.push_back(l);

        ProposalDraft i;
        i.action = ProposedAction::ImplementIssue;
        i.rationale = "Bug is well-scoped; queue for the handoff coding loop.";
        i.payload = nlohmann::json{{"complexityHint", "medium"}, {"approachOutline", "Switch to shared_mutex."}};
        outDrafts.push_back(i);

        return true;
    }
};

// Whitelist of valid action enums for the post-triage row sanity check. The
// fixture only emits these three, so any other value surfaces a regression in
// either the mocks or the controller's persistence path.
bool ActionIsKnown(ProposedAction a) {
    switch (a) {
    case ProposedAction::CommentAdd:
    case ProposedAction::LabelAdd:
    case ProposedAction::LabelRemove:
    case ProposedAction::AssigneeSet:
    case ProposedAction::StateTransition:
    case ProposedAction::DerivedTicketCreate:
    case ProposedAction::ImplementIssue:
        return true;
    case ProposedAction::Unknown:
        break;
    }
    return false;
}

} // namespace

class AgentTriageScenario : public IScenario {
  public:
    std::string Name() const override { return "agent-triage-roundtrip"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frameLimit_ = std::max(8, args.value("frames", 12));
        try {
            github_ = std::make_unique<ScenarioMockGitHub>();
            inference_ = std::make_unique<ScenarioMockInference>();
            store_ = std::make_unique<AgentProposalStore>(":memory:");
            ctrl_ = std::make_unique<AgenticTriageController>(github_.get(), inference_.get(), store_.get());
        } catch (const std::exception& ex) {
            outErr = std::string("scenario setup failed: ") + ex.what();
            return;
        } catch (...) {
            outErr = "scenario setup failed: unknown exception";
            return;
        }
        state_ = State::WaitingForBatch;
        LOG_INFO("AgentTriageScenario: wired mocks + controller + :memory: store");
    }

    void OnFrame(AppController& /*app*/, int frameIndex) override {
        if (state_ == State::Done)
            return;
        // Bundle C CR#233:207 — once a transition failed fatally, latch and
        // refuse to advance the state machine. The previous shape kept stepping
        // through subsequent OnFrame calls after RunBatch / a transition set
        // state_ = State::Done due to an inserted error, which could re-fire
        // assertions on a known-bad store + double-count `errors_`.
        if (fatalFailureRecorded_) {
            state_ = State::Done;
            return;
        }

        switch (state_) {
        case State::WaitingForBatch:
            RunBatch();
            if (state_ == State::Done) {
                fatalFailureRecorded_ = true;
                return;
            }
            state_ = State::WaitingForInitialQuery;
            break;
        case State::WaitingForInitialQuery:
            AssertInitialRows();
            if (state_ == State::Done) {
                fatalFailureRecorded_ = true;
                return;
            }
            state_ = State::WaitingForApproval;
            break;
        case State::WaitingForApproval:
            TransitionFirst(AgentProposalState::Approved);
            state_ = State::WaitingForRejection;
            break;
        case State::WaitingForRejection:
            TransitionSecond(AgentProposalState::Rejected);
            state_ = State::Done;
            break;
        default:
            break;
        }
        (void)frameIndex;
    }

    bool IsDone(int frameIndex) const override { return state_ == State::Done || frameIndex >= frameLimit_; }

    void OnCancel(AppController& /*app*/) override { Teardown(); }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        Teardown();
        nlohmann::json out;
        out["scenario"] = Name();
        out["state"] = StateName(state_);
        out["proposals_inserted"] = batch_.proposalsInserted;
        out["total_issues_scanned"] = batch_.totalIssuesScanned;
        out["approved_id"] = approvedId_;
        out["rejected_id"] = rejectedId_;
        out["state_transitions_ok"] = stateTransitionsOk_;
        out["passed"] =
            (errors_.empty() && state_ == State::Done && stateTransitionsOk_ && batch_.proposalsInserted > 0);
        out["errors"] = errors_;
        // Perf-rows emit so `scripts/dev/perf-baseline.sh init agent-triage-roundtrip`
        // captures a baseline (per the 8-of-15 retrofit in
        // docs/backlog/agent-self-improvement/tooling.md). Pattern mirrors
        // PriorityGridScrollScenario::OnFinish.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        nlohmann::json rowsJson = nlohmann::json::array();
        std::transform(rows.begin(), rows.end(), std::back_inserter(rowsJson), [](const UiPerfRow& r) {
            return nlohmann::json{
                {"name", r.name},
                {"lastTotalMs", r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs", r.maxMs},
                {"calls", r.calls},
                {"emaAvgMs", r.emaAvgMs},
            };
        });
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    enum class State {
        Initial,
        WaitingForBatch,
        WaitingForInitialQuery,
        WaitingForApproval,
        WaitingForRejection,
        Done,
    };

    static const char* StateName(State s) {
        switch (s) {
        case State::Initial:
            return "Initial";
        case State::WaitingForBatch:
            return "WaitingForBatch";
        case State::WaitingForInitialQuery:
            return "WaitingForInitialQuery";
        case State::WaitingForApproval:
            return "WaitingForApproval";
        case State::WaitingForRejection:
            return "WaitingForRejection";
        case State::Done:
            return "Done";
        }
        return "Unknown";
    }

    void RunBatch() {
        std::string err;
        if (!ctrl_->TriageBatch("github", "fake/repo", batch_, err)) {
            errors_.push_back("TriageBatch failed: " + err);
            state_ = State::Done;
            return;
        }
        std::transform(
            batch_.perIssueErrors.begin(), batch_.perIssueErrors.end(), std::back_inserter(errors_),
            [](const std::pair<std::string, std::string>& kv) { return "per-issue " + kv.first + ": " + kv.second; });
        if (batch_.proposalsInserted <= 0) {
            errors_.push_back("TriageBatch inserted zero proposals — fixture / mock mismatch?");
            state_ = State::Done;
        }
    }

    void AssertInitialRows() {
        AgentProposalStore::Filter f;
        std::vector<AgentProposal> rows;
        std::string err;
        if (!store_->Query(f, rows, err)) {
            errors_.push_back("initial Query failed: " + err);
            state_ = State::Done;
            return;
        }
        if (rows.empty()) {
            errors_.push_back("expected at least one row after TriageBatch.");
            state_ = State::Done;
            return;
        }
        for (const auto& r : rows) {
            if (r.state != AgentProposalState::Pending) {
                errors_.push_back("non-Pending state after insert for id=" + std::to_string(r.id));
                stateTransitionsOk_ = false;
            }
            if (r.sourceTracker != "github") {
                errors_.push_back("unexpected sourceTracker for id=" + std::to_string(r.id));
                stateTransitionsOk_ = false;
            }
            if (!ActionIsKnown(r.action)) {
                errors_.push_back("unknown action enum for id=" + std::to_string(r.id));
                stateTransitionsOk_ = false;
            }
        }
        // Capture two ids for the upcoming transitions. We sort by id so the
        // approved/rejected ids are deterministic across runs — SQLite ROWID
        // assignment is monotonic but the iteration order of Query is not
        // contractually guaranteed to be insertion order.
        std::vector<int64_t> ids;
        ids.reserve(rows.size());
        std::transform(rows.begin(), rows.end(), std::back_inserter(ids), [](const AgentProposal& r) { return r.id; });
        std::sort(ids.begin(), ids.end());
        if (ids.size() >= 1)
            candidateApproveId_ = ids[0];
        if (ids.size() >= 2)
            candidateRejectId_ = ids[1];
    }

    void TransitionFirst(AgentProposalState target) {
        if (candidateApproveId_ <= 0) {
            errors_.push_back("no candidate id for approve transition.");
            stateTransitionsOk_ = false;
            return;
        }
        std::string err;
        if (!store_->Transition(candidateApproveId_, target, std::string(), err)) {
            errors_.push_back("approve Transition failed: " + err);
            stateTransitionsOk_ = false;
            return;
        }
        AgentProposal p;
        if (!store_->Find(candidateApproveId_, p, err)) {
            errors_.push_back("approve Find failed: " + err);
            stateTransitionsOk_ = false;
            return;
        }
        if (p.state != AgentProposalState::Approved) {
            errors_.push_back("approve row state mismatch.");
            stateTransitionsOk_ = false;
            return;
        }
        approvedId_ = candidateApproveId_;
    }

    void TransitionSecond(AgentProposalState target) {
        if (candidateRejectId_ <= 0) {
            errors_.push_back("no candidate id for reject transition.");
            stateTransitionsOk_ = false;
            return;
        }
        std::string err;
        if (!store_->Transition(candidateRejectId_, target, std::string(), err)) {
            errors_.push_back("reject Transition failed: " + err);
            stateTransitionsOk_ = false;
            return;
        }
        AgentProposal p;
        if (!store_->Find(candidateRejectId_, p, err)) {
            errors_.push_back("reject Find failed: " + err);
            stateTransitionsOk_ = false;
            return;
        }
        if (p.state != AgentProposalState::Rejected) {
            errors_.push_back("reject row state mismatch.");
            stateTransitionsOk_ = false;
            return;
        }
        rejectedId_ = candidateRejectId_;
    }

    void Teardown() {
        // Tear down in dependency order: controller holds raw pointers into
        // the three deps below, so it must die first or its dtor reads stale
        // memory. The unique_ptr resets handle that for us when the scenario
        // unwinds, but explicit Teardown lets OnCancel reach the same path.
        ctrl_.reset();
        store_.reset();
        inference_.reset();
        github_.reset();
    }

    State state_ = State::Initial;
    int frameLimit_ = 12;
    std::unique_ptr<ScenarioMockGitHub> github_;
    std::unique_ptr<ScenarioMockInference> inference_;
    std::unique_ptr<AgentProposalStore> store_;
    std::unique_ptr<AgenticTriageController> ctrl_;
    AgenticTriageController::BatchResult batch_;
    int64_t candidateApproveId_ = 0;
    int64_t candidateRejectId_ = 0;
    int64_t approvedId_ = 0;
    int64_t rejectedId_ = 0;
    bool stateTransitionsOk_ = true;
    bool fatalFailureRecorded_ = false;
    std::vector<std::string> errors_;
};

} // namespace cmd
} // namespace smatchet

/// Factory called from AppController::Initialize behind an
/// `#if defined(SMATCHET_WITH_AGENTIC)` guard. The TU is excluded from
/// CORE_SOURCES on the OFF build, so the extern in AppController.cpp must
/// be ifdef-wrapped to match.
std::unique_ptr<smatchet::cmd::IScenario> MakeAgentTriageScenario() {
    // Up-cast factory: construct the concrete scenario and hand it back through the
    // IScenario base. `make_unique<Derived>()` returns a `unique_ptr<Derived>` which
    // implicitly converts to `unique_ptr<Base>` on return via the converting ctor.
    return std::make_unique<smatchet::cmd::AgentTriageScenario>();
}
