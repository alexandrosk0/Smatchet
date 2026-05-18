// AgenticHandoffController — drives one HarnessRunState lifecycle against a
// `FakeRunner` that scripts the state-change callbacks + RunResult outcome.
// The test rig uses a real `AgentProposalStore(":memory:")` (already covered
// by AgentProposalStore.test.cpp) and a captured audit-sink vector so every
// transition can be asserted on.

#include "AgenticHandoffController.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"
#include "CodingHarnessTypes.h"
#include "HarnessRunState.h"
#include "ICodingHarnessRunner.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using ::smatchet::agentic::AgenticHandoffController;
using ::smatchet::agentic::AuditSink;
using ::AgenticInferenceClientPure::ProposedAction;
using ::CodingHarness::RunState;

namespace {

// FakeRunner — scriptable test double. Two modes:
//   - HappyPath: Spawn fires Running -> Complete via the state callback,
//     returns ok=true + a fake PR URL.
//   - Clarify: Spawn fires Running -> AwaitingUser, waits up to 200ms for
//     Resume(), then fires Running -> Complete.
//   - Error: Spawn fires Failed via callback, returns false.
//   - SleepCancel: Spawn sits in a 200ms wait checking the cancel atom; on
//     cancel returns false with result.errorMessage="cancelled" + the
//     controller sees cancelToken==true and records Cancelled.
class FakeRunner : public CodingHarness::IRunner {
  public:
    enum class Mode { HappyPath, Clarify, Error, SleepCancel };

    explicit FakeRunner(Mode m) : mode_(m) {}

    bool Probe(std::string& outError) override {
        outError.clear();
        return true;
    }

    bool Spawn(const CodingHarness::Seed& seed, const std::string& worktreeDir, DeltaCallback onDelta,
               StateChangeCallback onStateChange, std::shared_ptr<std::atomic<bool>> cancelToken,
               CodingHarness::RunResult& outResult, std::string& outError) override {
        ++spawnCalls;
        capturedSeed = seed;
        capturedWorktree = worktreeDir;
        (void)onDelta;
        outError.clear();

        switch (mode_) {
        case Mode::HappyPath: {
            // Happy path crosses PrOpen — `Running -> Complete` is not an
            // allowed transition (Complete requires PrOpen). The runner
            // emits PrOpen when the harness writes PR_URL.txt; the test
            // fakes the same sequence.
            if (onStateChange) onStateChange("Running");
            if (onStateChange) onStateChange("PrOpen");
            if (onStateChange) onStateChange("Complete");
            outResult.ok = true;
            outResult.prUrl = "https://example.invalid/pr/1";
            outResult.filesChanged = 2;
            return true;
        }
        case Mode::Clarify: {
            if (onStateChange) onStateChange("Running");
            if (onStateChange) onStateChange("AwaitingUser");
            // Wait briefly for Resume() to flip resumeReceived_; the test
            // posts ProvideClarification on another thread.
            for (int i = 0; i < 50 && !resumeReceived_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (onStateChange) onStateChange("Running");
            if (onStateChange) onStateChange("PrOpen");
            if (onStateChange) onStateChange("Complete");
            outResult.ok = true;
            outResult.prUrl = "https://example.invalid/pr/2";
            return true;
        }
        case Mode::Error: {
            if (onStateChange) onStateChange("Running");
            if (onStateChange) onStateChange("Failed");
            outResult.ok = false;
            outResult.errorMessage = "scripted-failure";
            return false;
        }
        case Mode::SleepCancel: {
            if (onStateChange) onStateChange("Running");
            for (int i = 0; i < 200; ++i) {
                if (cancelToken && cancelToken->load()) {
                    outResult.ok = false;
                    outResult.errorMessage = "cancelled";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            outResult.ok = false;
            outResult.errorMessage = "timeout-no-cancel";
            return false;
        }
        }
        return false;
    }

    bool Resume(const CodingHarness::ClarificationResponse& response,
                std::shared_ptr<std::atomic<bool>> /*cancelToken*/, std::string& outError) override {
        ++resumeCalls;
        capturedAnswer = response.answer;
        resumeReceived_.store(true);
        outError.clear();
        return true;
    }

    std::string Name() const override { return "fake-runner"; }

    Mode mode_;
    std::atomic<bool> resumeReceived_{false};
    int spawnCalls = 0;
    int resumeCalls = 0;
    CodingHarness::Seed capturedSeed;
    std::string capturedWorktree;
    std::string capturedAnswer;
};

// Audit-sink capture. One entry per call, in invocation order.
struct AuditEntry {
    std::string action;
    std::string source;
    std::string issueKey;
    bool success = false;
    std::string errorMessage;
    nlohmann::json data;
};

// Thread-safe capture so tests that drive the worker on a side thread can
// snapshot the audit log without UB. `mu` is wrapped in a shared_ptr so the
// callback's capture lifetime is well-defined.
struct CapturingSink {
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<AuditEntry>> rows = std::make_shared<std::vector<AuditEntry>>();

    std::vector<AuditEntry> Copy() const {
        std::lock_guard<std::mutex> lk(*mu);
        return *rows;
    }
    std::size_t Size() const {
        std::lock_guard<std::mutex> lk(*mu);
        return rows->size();
    }
};

AuditSink MakeCapturingSink(CapturingSink& sink) {
    auto mu = sink.mu;
    auto rows = sink.rows;
    return [mu, rows](const std::string& a, const std::string& s, const std::string& k, bool ok,
                      const std::string& e, const nlohmann::json& d) {
        AuditEntry row;
        row.action = a;
        row.source = s;
        row.issueKey = k;
        row.success = ok;
        row.errorMessage = e;
        row.data = d;
        std::lock_guard<std::mutex> lk(*mu);
        rows->push_back(std::move(row));
    };
}

// Inserts an ImplementIssue proposal + returns its id.
std::int64_t InsertImplementIssue(AgentProposalStore& store, const std::string& issueKey) {
    AgentProposal p;
    p.sourceTracker = "github";
    p.issueKey = issueKey;
    p.action = ProposedAction::ImplementIssue;
    p.rationale = "fix the crash";
    p.payload = nlohmann::json{{"complexityHint", "low"}, {"approachOutline", "guard the nullptr"}};
    std::string err;
    REQUIRE(store.Insert(p, err));
    REQUIRE(p.id > 0);
    return p.id;
}

std::int64_t InsertCommentAdd(AgentProposalStore& store, const std::string& issueKey) {
    AgentProposal p;
    p.sourceTracker = "github";
    p.issueKey = issueKey;
    p.action = ProposedAction::CommentAdd;
    p.rationale = "ack the reporter";
    p.payload = nlohmann::json{{"body", "thanks for the report"}};
    std::string err;
    REQUIRE(store.Insert(p, err));
    return p.id;
}

} // namespace

TEST_CASE("AgenticHandoffController::Start happy path drives Pending->Spawning->Running->Complete") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#42");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));
    CHECK(err.empty());
    CHECK(h.proposalId == pid);
    CHECK(h.branchName == AgenticHandoffController::BuildBranchName(pid, "smatchet/example#42"));
    CHECK(h.worktreeDir == AgenticHandoffController::BuildWorktreeDir(pid));

    // Drive the worker synchronously on the test thread.
    REQUIRE(ctrl.RunSpawnSynchronouslyForTests(pid, err));

    const auto all = ctrl.SnapshotAllForTests();
    REQUIRE(all.size() == 1);
    CHECK(all[0].state == RunState::Complete);
    CHECK(all[0].prUrl == "https://example.invalid/pr/1");

    // Audit ordering: Pending->Spawning, Spawning->Running, Running->PrOpen,
    // PrOpen->Complete. The runner emits terminal callbacks too, but the
    // controller filters them (see AgenticHandoffController.cpp §
    // onStateChange) so the worker's post-Spawn ControllerTransition owns
    // the final audit row.
    const auto audit = sink.Copy();
    REQUIRE(audit.size() == 4);
    CHECK(audit[0].action == "HandoffStateTransition");
    CHECK(audit[0].source == "agentic");
    CHECK(audit[0].issueKey == "smatchet/example#42");
    CHECK(audit[0].data["fromState"] == "Pending");
    CHECK(audit[0].data["toState"] == "Spawning");
    CHECK(audit[1].data["fromState"] == "Spawning");
    CHECK(audit[1].data["toState"] == "Running");
    CHECK(audit[2].data["fromState"] == "Running");
    CHECK(audit[2].data["toState"] == "PrOpen");
    CHECK(audit[3].data["fromState"] == "PrOpen");
    CHECK(audit[3].data["toState"] == "Complete");
    CHECK(audit[3].success == true);

    // SnapshotActive filters terminal entries (Complete is sinky).
    CHECK(ctrl.SnapshotActive().empty());

    // Runner saw the seed we built.
    CHECK(runner.spawnCalls == 1);
    CHECK(runner.capturedSeed.proposalId == pid);
    CHECK(runner.capturedSeed.targetBranch == h.branchName);
    CHECK(runner.capturedSeed.issueKey == "smatchet/example#42");
    CHECK(runner.capturedSeed.complexityHint == "low");
    CHECK(runner.capturedSeed.approachOutline == "guard the nullptr");
}

TEST_CASE("AgenticHandoffController::Start rejects non-ImplementIssue proposal") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertCommentAdd(store, "smatchet/example#7");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    CHECK_FALSE(ctrl.Start(pid, h, err));
    CHECK(err.find("not ImplementIssue") != std::string::npos);
    CHECK(sink.Size() == 0); // precondition failures emit no audit row
    CHECK(runner.spawnCalls == 0);
}

TEST_CASE("AgenticHandoffController::Start fails for non-existent proposalId") {
    AgentProposalStore store(":memory:");
    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    CHECK_FALSE(ctrl.Start(99999, h, err));
    CHECK(err.find("lookup failed") != std::string::npos);
    CHECK(sink.Size() == 0);
}

TEST_CASE("AgenticHandoffController::Start rejects already-in-flight proposalId") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#9");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));
    // Without running the worker the state is Spawning — non-terminal.
    AgenticHandoffController::ActiveHandoff h2;
    CHECK_FALSE(ctrl.Start(pid, h2, err));
    CHECK(err.find("already in flight") != std::string::npos);
}

TEST_CASE("AgenticHandoffController — clarification round-trip flows AwaitingUser -> Running -> Complete") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#3");

    FakeRunner runner(FakeRunner::Mode::Clarify);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    // Run the worker on a side thread so the test can call ProvideClarification
    // while Spawn is parked in AwaitingUser.
    std::thread workerThread([&]() {
        std::string werr;
        ctrl.RunSpawnSynchronouslyForTests(pid, werr);
    });

    // Poll for AwaitingUser via the controller's mutex-protected snapshot.
    // (Reading the audit vector from the test thread while the worker pushes
    // to it would be a data race.) Bounded so a regression fails fast.
    bool sawAwaiting = false;
    for (int i = 0; i < 200 && !sawAwaiting; ++i) {
        const auto snap = ctrl.SnapshotAllForTests();
        if (!snap.empty() && snap[0].state == RunState::AwaitingUser) {
            sawAwaiting = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(sawAwaiting);

    REQUIRE(ctrl.ProvideClarification(pid, "yes, do that", err));
    CHECK(err.empty());
    CHECK(runner.resumeCalls == 1);
    CHECK(runner.capturedAnswer == "yes, do that");

    workerThread.join();

    const auto all = ctrl.SnapshotAllForTests();
    REQUIRE(all.size() == 1);
    CHECK(all[0].state == RunState::Complete);

    // Pending->Spawning, Spawning->Running, Running->AwaitingUser,
    // AwaitingUser->Running, Running->PrOpen, PrOpen->Complete = 6 audit rows.
    const auto audit = sink.Copy();
    CHECK(audit.size() == 6);
    CHECK(audit[0].data["toState"] == "Spawning");
    CHECK(audit[1].data["toState"] == "Running");
    CHECK(audit[2].data["toState"] == "AwaitingUser");
    CHECK(audit[3].data["toState"] == "Running");
    CHECK(audit[4].data["toState"] == "PrOpen");
    CHECK(audit[5].data["toState"] == "Complete");
}

TEST_CASE("AgenticHandoffController — runner failure transitions to Failed and records error") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#11");

    FakeRunner runner(FakeRunner::Mode::Error);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));
    REQUIRE(ctrl.RunSpawnSynchronouslyForTests(pid, err));

    const auto all = ctrl.SnapshotAllForTests();
    REQUIRE(all.size() == 1);
    CHECK(all[0].state == RunState::Failed);
    CHECK(all[0].lastError == "scripted-failure");

    // Audit shape: Pending->Spawning, Spawning->Running, Running->Failed.
    // The runner emits a "Failed" state callback too, but the controller
    // filters terminal-state callbacks (see AgenticHandoffController.cpp);
    // the worker's post-Spawn ControllerTransition carries the richer
    // RunResult.errorMessage and is the recorded entry.
    const auto audit = sink.Copy();
    CHECK(audit.size() == 3);
    CHECK(audit[2].data["toState"] == "Failed");
    CHECK(audit[2].success == false);
    CHECK(audit[2].data["errorMessage"] == "scripted-failure");
}

TEST_CASE("AgenticHandoffController::Cancel raises atom and surfaces Cancelled") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#5");

    FakeRunner runner(FakeRunner::Mode::SleepCancel);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    std::thread workerThread([&]() {
        std::string werr;
        ctrl.RunSpawnSynchronouslyForTests(pid, werr);
    });

    // Let the runner enter its sleep loop, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(ctrl.Cancel(pid, err));
    CHECK(err.empty());

    workerThread.join();

    const auto all = ctrl.SnapshotAllForTests();
    REQUIRE(all.size() == 1);
    CHECK(all[0].state == RunState::Cancelled);
    CHECK(all[0].lastError.find("cancelled") != std::string::npos);

    // Cancel cannot be re-requested on a terminal record.
    CHECK_FALSE(ctrl.Cancel(pid, err));
    CHECK(err.find("terminal") != std::string::npos);
}

TEST_CASE("AgenticHandoffController::BuildShortSlug obeys 32-char cap and kebab rules") {
    // Empty / pathological inputs fall back to "issue".
    CHECK(AgenticHandoffController::BuildShortSlug("") == "issue");
    CHECK(AgenticHandoffController::BuildShortSlug("####") == "issue");
    // Mixed-case + separators kebab to lower-ascii dashes.
    CHECK(AgenticHandoffController::BuildShortSlug("Smatchet/Example#42") == "smatchet-example-42");
    // 32-char cap with trailing dashes trimmed.
    const std::string long_key = "very-very-very-very-very-very-very-long-key#9999";
    const std::string slug = AgenticHandoffController::BuildShortSlug(long_key);
    CHECK(slug.size() <= 32);
    CHECK(slug.find_first_of("/#") == std::string::npos);
    CHECK(slug.back() != '-');
}

TEST_CASE("AgenticHandoffController::BuildBranchName + BuildWorktreeDir match plan decisions") {
    CHECK(AgenticHandoffController::BuildBranchName(42, "smatchet/example#42") ==
          "agent/42/smatchet-example-42");
    CHECK(AgenticHandoffController::BuildWorktreeDir(42) == ".claude/worktrees/agent-42");
}

#endif // SMATCHET_WITH_AGENTIC
