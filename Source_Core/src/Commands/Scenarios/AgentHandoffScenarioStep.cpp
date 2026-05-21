// AgentHandoffScenarioStep — H10 end-to-end regression scenario for the
// agentic handoff lifecycle. Mirrors the shape of AgentTriageScenarioStep
// (the T8 triage-side analogue) so a CI run can assert the full handoff
// FSM via the `scenario.run --name=agent-handoff-roundtrip` CLI without
// requiring a real `claude` install, real GitHub PAT, or real cpr traffic.
//
// What this exercises:
//   - Insert an ImplementIssue proposal in :memory: AgentProposalStore
//   - Approve it (Pending -> Approved)
//   - Construct AgenticHandoffController against a scripted FakeRunner
//     (no real subprocess) + null worker dispatcher (drives Spawn() via
//     RunSpawnSynchronouslyForTests on the scenario's thread, keeping the
//     scenario single-threaded + deterministic)
//   - Call Start() to begin the handoff (transitions Pending -> Spawning)
//   - Run the synchronous-for-tests driver to walk through Running -> PrOpen
//     -> Complete
//   - Assert all four expected FSM transitions audit-fired in order
//   - Assert SnapshotAllForTests carries the terminal Complete row with the
//     PR URL the runner reported
//
// Frame-state machine:
//   OnStart            — wire AgentProposalStore + AgenticHandoffController
//                        + FakeRunner + capturing audit sink + insert one
//                        ImplementIssue proposal + Approve it
//   OnFrame[0]         — Start handoff (Pending -> Spawning)
//   OnFrame[1]         — RunSpawnSynchronouslyForTests (walks the FSM to
//                        Complete via the FakeRunner's HappyPath script)
//   OnFrame[2]         — Snapshot + assert state==Complete + prUrl populated
//   IsDone             — true after frame 2 OR on assertion failure
//   OnFinish           — JSON {passed, proposalId, finalState, prUrl,
//                              transitions: [...], errors: [...]}
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC in
// the root CMakeLists.txt; the factory call in AppController.cpp wraps in
// `#if defined(SMATCHET_WITH_AGENTIC)` so the OFF build has no dangling
// reference to MakeAgentHandoffScenario.

#include "Commands/Scenarios/IScenario.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticHandoffController.h"
#include "AgenticInferenceClientPure.h"
#include "CodingHarnessTypes.h"
#include "HarnessRunState.h"
#include "ICodingHarnessRunner.h"
#include "Logger.h"
#include "UiPerfMonitor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

using ::AgenticInferenceClientPure::ProposedAction;
using ::CodingHarness::RunState;
using ::smatchet::agentic::AgenticHandoffController;
using ::smatchet::agentic::AuditSink;

// Scripted runner — same shape as the H4 test FakeRunner (HappyPath mode)
// but contained in this TU so the scenario does not pull in test/_helpers
// or the gtest/doctest rig. The runner emits the canonical happy-path FSM:
// Running -> PrOpen -> Complete via the state callback, then returns ok=true
// with a fake PR URL.
class ScenarioFakeRunner : public ::CodingHarness::IRunner {
  public:
    bool Probe(std::string& outError) override {
        outError.clear();
        return true;
    }

    bool Spawn(const ::CodingHarness::Seed& seed, const std::string& worktreeDir, DeltaCallback onDelta,
               StateChangeCallback onStateChange, std::shared_ptr<std::atomic<bool>> cancelToken,
               ::CodingHarness::RunResult& outResult, std::string& outError) override {
        (void)onDelta;
        (void)cancelToken;
        ++spawnCalls;
        capturedSeed = seed;
        capturedWorktree = worktreeDir;
        outError.clear();
        if (onStateChange) {
            onStateChange("Running");
            onStateChange("PrOpen");
            onStateChange("Complete");
        }
        outResult.ok = true;
        outResult.prUrl = "https://example.invalid/pr/h10-scenario";
        outResult.filesChanged = 1;
        outResult.linesAdded = 5;
        return true;
    }

    bool Resume(const ::CodingHarness::ClarificationResponse&, std::shared_ptr<std::atomic<bool>>,
                std::string& outError) override {
        outError.clear();
        return true;
    }

    std::string Name() const override { return "scenario-fake-runner"; }

    int spawnCalls = 0;
    ::CodingHarness::Seed capturedSeed;
    std::string capturedWorktree;
};

} // namespace

class AgentHandoffScenario : public IScenario {
  public:
    std::string Name() const override { return "agent-handoff-roundtrip"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frameLimit_ = std::max(6, args.value("frames", 10));
        try {
            store_ = std::make_unique<AgentProposalStore>(":memory:");
            runner_ = std::make_unique<ScenarioFakeRunner>();

            // Capture audit sink — every FSM transition records here so the
            // OnFinish JSON can surface the full transition trail.
            ctrl_ = std::make_unique<AgenticHandoffController>(
                ::smatchet::agentic::WorkerDispatcher(), runner_.get(), store_.get(),
                [this](const std::string& action, const std::string& source, const std::string& issueKey, bool ok,
                       const std::string& errorMsg, const nlohmann::json& data) {
                    AuditCapture row;
                    row.action = action;
                    row.source = source;
                    row.issueKey = issueKey;
                    row.success = ok;
                    row.errorMessage = errorMsg;
                    row.data = data;
                    std::lock_guard<std::mutex> lk(auditMu_);
                    audit_.push_back(std::move(row));
                });

            // Insert + approve an ImplementIssue proposal.
            AgentProposal p;
            p.sourceTracker = "github";
            p.issueKey = "smatchet/example#h10-scenario";
            p.action = ProposedAction::ImplementIssue;
            p.rationale = "scenario: implement the fix";
            p.payload = nlohmann::json{{"complexityHint", "low"}, {"approachOutline", "the obvious fix"}};
            std::string err;
            if (!store_->Insert(p, err)) {
                outErr = std::string("scenario setup: Insert failed: ") + err;
                return;
            }
            proposalId_ = p.id;
            if (!store_->Transition(proposalId_, AgentProposalState::Approved, std::string(), err)) {
                outErr = std::string("scenario setup: Approve transition failed: ") + err;
                return;
            }
        } catch (const std::exception& ex) {
            outErr = std::string("scenario setup failed: ") + ex.what();
            return;
        } catch (...) {
            outErr = "scenario setup failed: unknown exception";
            return;
        }
        state_ = State::WaitingForStart;
        LOG_INFO("AgentHandoffScenario: wired controller + store + scripted runner (proposalId=%lld)",
                 static_cast<long long>(proposalId_));
    }

    void OnFrame(AppController& /*app*/, int frameIndex) override {
        if (state_ == State::Done)
            return;
        if (fatalFailureRecorded_) {
            state_ = State::Done;
            return;
        }

        switch (state_) {
        case State::WaitingForStart:
            DoStart();
            if (state_ == State::Done) {
                fatalFailureRecorded_ = true;
                return;
            }
            state_ = State::WaitingForDriver;
            break;
        case State::WaitingForDriver:
            DoRunSynchronously();
            if (state_ == State::Done) {
                fatalFailureRecorded_ = true;
                return;
            }
            state_ = State::WaitingForAssertions;
            break;
        case State::WaitingForAssertions:
            DoAssertFinalState();
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
        out["proposalId"] = proposalId_;
        out["finalState"] = ::CodingHarness::RunStateToString(finalState_);
        out["prUrl"] = finalPrUrl_;

        // Surface the audit trail as a compact transitions array so a reader
        // can verify the FSM walked exactly Pending->Spawning->Running->
        // PrOpen->Complete.
        nlohmann::json transitions = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lk(auditMu_);
            for (const auto& a : audit_) {
                if (a.action != "HandoffStateTransition")
                    continue;
                nlohmann::json t;
                t["from"] = a.data.value("fromState", std::string());
                t["to"] = a.data.value("toState", std::string());
                t["ok"] = a.success;
                if (!a.errorMessage.empty())
                    t["error"] = a.errorMessage;
                transitions.push_back(std::move(t));
            }
        }
        out["transitions"] = transitions;
        out["errors"] = errors_;
        out["passed"] =
            (errors_.empty() && state_ == State::Done && finalState_ == RunState::Complete && !finalPrUrl_.empty());
        // Perf-rows emit so `scripts/dev/perf-baseline.sh init agent-handoff-roundtrip`
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
        WaitingForStart,
        WaitingForDriver,
        WaitingForAssertions,
        Done,
    };

    static const char* StateName(State s) {
        switch (s) {
        case State::Initial:
            return "Initial";
        case State::WaitingForStart:
            return "WaitingForStart";
        case State::WaitingForDriver:
            return "WaitingForDriver";
        case State::WaitingForAssertions:
            return "WaitingForAssertions";
        case State::Done:
            return "Done";
        }
        return "Unknown";
    }

    void DoStart() {
        AgenticHandoffController::ActiveHandoff h;
        std::string err;
        if (!ctrl_->Start(proposalId_, h, err)) {
            errors_.push_back("Start failed: " + err);
            state_ = State::Done;
            return;
        }
        if (h.proposalId != proposalId_) {
            errors_.push_back("Start returned wrong proposalId");
            state_ = State::Done;
            return;
        }
    }

    void DoRunSynchronously() {
        std::string err;
        if (!ctrl_->RunSpawnSynchronouslyForTests(proposalId_, err)) {
            errors_.push_back("RunSpawnSynchronouslyForTests failed: " + err);
            state_ = State::Done;
            return;
        }
    }

    void DoAssertFinalState() {
        const auto snap = ctrl_->SnapshotAllForTests();
        const auto it = std::find_if(snap.begin(), snap.end(), [&](const AgenticHandoffController::ActiveHandoff& a) {
            return a.proposalId == proposalId_;
        });
        const AgenticHandoffController::ActiveHandoff* found = (it != snap.end()) ? &(*it) : nullptr;
        if (!found) {
            errors_.push_back("post-run: handoff record missing from SnapshotAllForTests");
            return;
        }
        finalState_ = found->state;
        finalPrUrl_ = found->prUrl;
        if (found->state != RunState::Complete) {
            errors_.push_back(std::string("post-run: final state != Complete (got ") +
                              ::CodingHarness::RunStateToString(found->state) + ")");
        }
        if (found->prUrl.empty()) {
            errors_.push_back("post-run: prUrl was not populated");
        }

        // Assert exactly 4 HandoffStateTransition audit rows in the expected
        // order: Pending->Spawning, Spawning->Running, Running->PrOpen,
        // PrOpen->Complete.
        std::vector<std::pair<std::string, std::string>> got;
        {
            std::lock_guard<std::mutex> lk(auditMu_);
            for (const auto& a : audit_) {
                if (a.action != "HandoffStateTransition")
                    continue;
                got.emplace_back(a.data.value("fromState", std::string()), a.data.value("toState", std::string()));
            }
        }
        const std::vector<std::pair<std::string, std::string>> expected = {
            {"Pending", "Spawning"},
            {"Spawning", "Running"},
            {"Running", "PrOpen"},
            {"PrOpen", "Complete"},
        };
        if (got != expected) {
            std::string detail = "post-run: FSM transitions != expected. got=[";
            for (std::size_t i = 0; i < got.size(); ++i) {
                if (i)
                    detail += ", ";
                detail += got[i].first + "->" + got[i].second;
            }
            detail += "]";
            errors_.push_back(std::move(detail));
        }
    }

    void Teardown() {
        // Tear down in dependency order — controller holds non-owning
        // pointers to runner + store.
        ctrl_.reset();
        runner_.reset();
        store_.reset();
    }

    struct AuditCapture {
        std::string action;
        std::string source;
        std::string issueKey;
        bool success = false;
        std::string errorMessage;
        nlohmann::json data;
    };

    State state_ = State::Initial;
    int frameLimit_ = 10;
    std::int64_t proposalId_ = 0;
    bool fatalFailureRecorded_ = false;
    RunState finalState_ = RunState::Pending;
    std::string finalPrUrl_;
    std::vector<std::string> errors_;

    std::unique_ptr<AgentProposalStore> store_;
    std::unique_ptr<ScenarioFakeRunner> runner_;
    std::unique_ptr<AgenticHandoffController> ctrl_;

    mutable std::mutex auditMu_;
    std::vector<AuditCapture> audit_;
};

} // namespace cmd
} // namespace smatchet

/// Factory called from AppController::Initialize behind an
/// `#if defined(SMATCHET_WITH_AGENTIC)` guard. The TU is excluded from
/// CORE_SOURCES on the OFF build, so the extern in AppController.cpp must
/// be ifdef-wrapped to match.
std::unique_ptr<smatchet::cmd::IScenario> MakeAgentHandoffScenario() {
    return std::make_unique<smatchet::cmd::AgentHandoffScenario>();
}
