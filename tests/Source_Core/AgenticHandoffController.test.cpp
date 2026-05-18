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
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using ::AgenticInferenceClientPure::ProposedAction;
using ::CodingHarness::RunState;
using ::smatchet::agentic::AgenticHandoffController;
using ::smatchet::agentic::AuditSink;
using ::smatchet::agentic::PostedComment;

namespace {

// (H5) mkdir -p the relative worktree path the controller builds. The path
// is `.claude/worktrees/agent-<id>` relative to cwd. We need the dir to
// exist before the FakeRunner writes CLARIFICATION_NEEDED.json (which the
// controller reads on the AwaitingUser transition). Cleanup deletes the
// individual file + the agent-<id> dir; we leave the `.claude/worktrees`
// parent in place — other tests may share it.
bool MkdirRecursive(const std::string& path) {
#if defined(_WIN32)
    std::string acc;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (!acc.empty()) {
                if (CreateDirectoryA(acc.c_str(), nullptr) == 0) {
                    if (GetLastError() != ERROR_ALREADY_EXISTS) {
                        return false;
                    }
                }
            }
            if (i < path.size()) {
                acc.push_back(path[i]);
            }
        } else {
            acc.push_back(path[i]);
        }
    }
    return true;
#else
    std::string acc;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty()) {
                if (mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            if (i < path.size()) {
                acc.push_back(path[i]);
            }
        } else {
            acc.push_back(path[i]);
        }
    }
    return true;
#endif
}

void RemoveFile(const std::string& path) {
#if defined(_WIN32)
    DeleteFileA(path.c_str());
#else
    unlink(path.c_str());
#endif
}

void RemoveDir(const std::string& path) {
#if defined(_WIN32)
    RemoveDirectoryA(path.c_str());
#else
    rmdir(path.c_str());
#endif
}

bool WriteText(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        return false;
    }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    return f.good();
}

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
            if (onStateChange)
                onStateChange("Running");
            if (onStateChange)
                onStateChange("PrOpen");
            if (onStateChange)
                onStateChange("Complete");
            outResult.ok = true;
            outResult.prUrl = "https://example.invalid/pr/1";
            outResult.filesChanged = 2;
            return true;
        }
        case Mode::Clarify: {
            if (onStateChange)
                onStateChange("Running");
            // (H5) Mirror the real runner: drop CLARIFICATION_NEEDED.json
            // into the worktree BEFORE firing AwaitingUser so the controller
            // can parse the question. The worktree dir must already exist
            // — tests use `MkdirRecursive(worktreeDir)` before driving the
            // worker so the runner+controller side both have a real dir to
            // touch. When clarificationQuestion is empty the FakeRunner
            // intentionally skips writing the file so a test can exercise
            // the "AwaitingUser without question text" fallback.
            if (!clarificationQuestion.empty()) {
                std::ofstream f(worktreeDir + "/CLARIFICATION_NEEDED.json", std::ios::binary | std::ios::trunc);
                if (f.is_open()) {
                    std::ostringstream os;
                    os << "{\"question\":\"" << clarificationQuestion << "\",\"timestampUnixSec\":0}";
                    const std::string body = os.str();
                    f.write(body.data(), static_cast<std::streamsize>(body.size()));
                }
            }
            if (onStateChange)
                onStateChange("AwaitingUser");
            // Wait briefly for Resume() to flip resumeReceived_; the test
            // posts ProvideClarification on another thread.
            for (int i = 0; i < 50 && !resumeReceived_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (onStateChange)
                onStateChange("Running");
            if (onStateChange)
                onStateChange("PrOpen");
            if (onStateChange)
                onStateChange("Complete");
            outResult.ok = true;
            outResult.prUrl = "https://example.invalid/pr/2";
            return true;
        }
        case Mode::Error: {
            if (onStateChange)
                onStateChange("Running");
            if (onStateChange)
                onStateChange("Failed");
            outResult.ok = false;
            outResult.errorMessage = "scripted-failure";
            return false;
        }
        case Mode::SleepCancel: {
            if (onStateChange)
                onStateChange("Running");
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
    // (H5) When non-empty AND mode_ == Clarify, the FakeRunner writes a
    // CLARIFICATION_NEEDED.json file with this question text under the
    // worktreeDir BEFORE emitting AwaitingUser, so the controller's
    // ReadAndStashClarificationQuestion picks it up.
    std::string clarificationQuestion;
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
    return [mu, rows](const std::string& a, const std::string& s, const std::string& k, bool ok, const std::string& e,
                      const nlohmann::json& d) {
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
    // AwaitingUser->Running, Running->PrOpen, PrOpen->Complete = 6 FSM
    // transitions. (H5) added one ClarificationProvided audit row alongside
    // ProvideClarification — assert the FSM transitions independently of the
    // sibling action so the test doesn't break when audit-trail expansion
    // adds adjacent rows in future phases.
    const auto audit = sink.Copy();
    std::vector<AuditEntry> fsmRows;
    for (const auto& r : audit) {
        if (r.action == "HandoffStateTransition") {
            fsmRows.push_back(r);
        }
    }
    REQUIRE(fsmRows.size() == 6);
    CHECK(fsmRows[0].data["toState"] == "Spawning");
    CHECK(fsmRows[1].data["toState"] == "Running");
    CHECK(fsmRows[2].data["toState"] == "AwaitingUser");
    CHECK(fsmRows[3].data["toState"] == "Running");
    CHECK(fsmRows[4].data["toState"] == "PrOpen");
    CHECK(fsmRows[5].data["toState"] == "Complete");
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
    CHECK(AgenticHandoffController::BuildBranchName(42, "smatchet/example#42") == "agent/42/smatchet-example-42");
    CHECK(AgenticHandoffController::BuildWorktreeDir(42) == ".claude/worktrees/agent-42");
}

// ─── H5 — clarification dual-channel + bot-filter ─────────────────────────

TEST_CASE("AgenticHandoffController H5 — bot-filter marker accessor + classifier") {
    // Single source of truth for the marker string. If anyone ever changes
    // this literal they will trip every in-flight handoff's poll loop —
    // surfacing it here documents the contract.
    CHECK(std::string(AgenticHandoffController::HandoffBotMarker()) == "<!-- smatchet-handoff -->");

    CHECK(AgenticHandoffController::IsHandoffBotComment("<!-- smatchet-handoff -->\n\n**foo**"));
    CHECK(AgenticHandoffController::IsHandoffBotComment("  <!-- smatchet-handoff -->\nfoo")); // leading whitespace
    CHECK_FALSE(AgenticHandoffController::IsHandoffBotComment("regular user reply"));
    CHECK_FALSE(AgenticHandoffController::IsHandoffBotComment("see <!-- smatchet-handoff --> below"));
    CHECK_FALSE(AgenticHandoffController::IsHandoffBotComment(""));
    // Case-sensitive — GitHub preserves comment body case verbatim. A
    // surprise upper-case marker should NOT match the bot prefix.
    CHECK_FALSE(AgenticHandoffController::IsHandoffBotComment("<!-- Smatchet-Handoff -->"));
}

TEST_CASE("AgenticHandoffController H5 — comment-body builders carry proposalId + marker") {
    const std::string clar = AgenticHandoffController::BuildClarificationCommentBody(42, "Which API style?");
    CHECK(clar.find("<!-- smatchet-handoff -->") == 0);
    CHECK(clar.find("proposal #42") != std::string::npos);
    CHECK(clar.find("Which API style?") != std::string::npos);
    CHECK(clar.find("Reply to this comment to answer.") != std::string::npos);
    CHECK(AgenticHandoffController::IsHandoffBotComment(clar));

    const std::string ans = AgenticHandoffController::BuildAnswerCommentBody(42, "Use FetchAsync");
    CHECK(ans.find("<!-- smatchet-handoff -->") == 0);
    CHECK(ans.find("proposal #42") != std::string::npos);
    CHECK(ans.find("User answered") != std::string::npos);
    CHECK(ans.find("Use FetchAsync") != std::string::npos);
    CHECK(AgenticHandoffController::IsHandoffBotComment(ans));
}

TEST_CASE("AgenticHandoffController H5 — clarification round-trip posts question + answer to GitHub") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#777");

    // Ensure the worktree dir exists so the FakeRunner can write the
    // clarification file the controller will then read.
    const std::string wt = AgenticHandoffController::BuildWorktreeDir(pid);
    REQUIRE(MkdirRecursive(wt));

    FakeRunner runner(FakeRunner::Mode::Clarify);
    runner.clarificationQuestion = "Which option, A or B?";
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    // Capture every CommentAdd via the poster seam.
    struct PostedRow {
        std::string issueKey;
        std::string body;
    };
    auto postedMu = std::make_shared<std::mutex>();
    auto posted = std::make_shared<std::vector<PostedRow>>();
    ctrl.SetGitHubCommentPoster(
        [postedMu, posted](const std::string& key, const std::string& body, std::string& outError) -> bool {
            (void)outError;
            std::lock_guard<std::mutex> lk(*postedMu);
            PostedRow r;
            r.issueKey = key;
            r.body = body;
            posted->push_back(std::move(r));
            return true;
        });

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    std::thread workerThread([&]() {
        std::string werr;
        ctrl.RunSpawnSynchronouslyForTests(pid, werr);
    });

    // Wait for AwaitingUser.
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

    // Question text was parsed + stashed.
    {
        const auto snap = ctrl.SnapshotAllForTests();
        REQUIRE(snap.size() == 1);
        CHECK(snap[0].lastClarificationQuestion == "Which option, A or B?");
        CHECK(snap[0].issueKey == "smatchet/example#777");
    }

    // First posted comment is the question — body carries the marker.
    {
        std::lock_guard<std::mutex> lk(*postedMu);
        REQUIRE(posted->size() == 1);
        CHECK((*posted)[0].issueKey == "smatchet/example#777");
        CHECK((*posted)[0].body.find("<!-- smatchet-handoff -->") == 0);
        CHECK((*posted)[0].body.find("Which option, A or B?") != std::string::npos);
        CHECK((*posted)[0].body.find("proposal #") != std::string::npos);
    }

    REQUIRE(ctrl.ProvideClarification(pid, "A", err));
    CHECK(err.empty());

    workerThread.join();

    // Second posted comment is the answer — body carries the marker + the answer.
    {
        std::lock_guard<std::mutex> lk(*postedMu);
        REQUIRE(posted->size() == 2);
        CHECK((*posted)[1].issueKey == "smatchet/example#777");
        CHECK((*posted)[1].body.find("<!-- smatchet-handoff -->") == 0);
        CHECK((*posted)[1].body.find("User answered") != std::string::npos);
        CHECK((*posted)[1].body.find("A") != std::string::npos);
    }

    // ClarificationProvided audit row should appear once with answer payload.
    bool foundProvided = false;
    for (const auto& row : sink.Copy()) {
        if (row.action == "ClarificationProvided") {
            foundProvided = true;
            CHECK(row.data.value("answer", std::string()) == "A");
            CHECK(row.data.value("postedToGithub", false) == true);
        }
    }
    CHECK(foundProvided);

    // lastClarificationQuestion cleared on resume.
    {
        const auto snap = ctrl.SnapshotAllForTests();
        REQUIRE(snap.size() == 1);
        CHECK(snap[0].state == RunState::Complete);
        CHECK(snap[0].lastClarificationQuestion.empty());
    }

    // Cleanup the worktree file + dir we created.
    RemoveFile(wt + "/CLARIFICATION_NEEDED.json");
    RemoveDir(wt);
}

TEST_CASE("AgenticHandoffController H5 — GitHub posting disabled by config flag") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#778");
    const std::string wt = AgenticHandoffController::BuildWorktreeDir(pid);
    REQUIRE(MkdirRecursive(wt));

    FakeRunner runner(FakeRunner::Mode::Clarify);
    runner.clarificationQuestion = "Style?";
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    auto postedMu = std::make_shared<std::mutex>();
    auto posted = std::make_shared<std::vector<std::string>>();
    ctrl.SetGitHubCommentPoster([postedMu, posted](const std::string&, const std::string& body, std::string&) -> bool {
        std::lock_guard<std::mutex> lk(*postedMu);
        posted->push_back(body);
        return true;
    });
    ctrl.SetGitHubClarificationEnabled(false); // flip OFF

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    std::thread workerThread([&]() {
        std::string werr;
        ctrl.RunSpawnSynchronouslyForTests(pid, werr);
    });

    // Wait for AwaitingUser to be observed by the controller.
    for (int i = 0; i < 200; ++i) {
        const auto snap = ctrl.SnapshotAllForTests();
        if (!snap.empty() && snap[0].state == RunState::AwaitingUser)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(ctrl.ProvideClarification(pid, "snake_case", err));
    workerThread.join();

    // Poster never invoked — the config gate held.
    {
        std::lock_guard<std::mutex> lk(*postedMu);
        CHECK(posted->empty());
    }

    // But ClarificationProvided audit still recorded with postedToGithub=false.
    bool foundProvided = false;
    for (const auto& row : sink.Copy()) {
        if (row.action == "ClarificationProvided") {
            foundProvided = true;
            CHECK(row.data.value("postedToGithub", true) == false);
        }
    }
    CHECK(foundProvided);

    RemoveFile(wt + "/CLARIFICATION_NEEDED.json");
    RemoveDir(wt);
}

TEST_CASE("AgenticHandoffController H5 — poll loop ignores bot comments + answers via user reply") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#779");
    const std::string wt = AgenticHandoffController::BuildWorktreeDir(pid);
    REQUIRE(MkdirRecursive(wt));

    FakeRunner runner(FakeRunner::Mode::Clarify);
    runner.clarificationQuestion = "Confirm rename?";
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    ctrl.SetGitHubCommentPoster([](const std::string&, const std::string&, std::string&) -> bool { return true; });

    // Fetcher script: first call returns the bot's own posted comment +
    // an unrelated bot prefix → poll loop should NOT dispatch. Second call
    // adds a user reply → poll loop should dispatch ProvideClarification.
    auto fetchCalls = std::make_shared<std::atomic<int>>(0);
    auto userReplyCreatedAt = std::make_shared<std::int64_t>(0);
    ctrl.SetGitHubCommentFetcher(
        [fetchCalls, userReplyCreatedAt](const std::string&, std::vector<PostedComment>& out, std::string&) -> bool {
            const int n = fetchCalls->fetch_add(1) + 1;
            out.clear();
            // The bot-authored question is always present (and must be filtered).
            PostedComment bot;
            bot.id = "1";
            bot.author = "smatchet-bot";
            bot.body = std::string(AgenticHandoffController::HandoffBotMarker()) +
                       "\n\n**Agent question (proposal #779):**\n\nConfirm rename?";
            bot.createdAtSec = 1; // always before any cursor we set
            out.push_back(bot);
            if (n >= 2) {
                // The user reply arrives on the 2nd fetch.
                PostedComment user;
                user.id = "2";
                user.author = "alice";
                user.body = "yes, rename";
                user.createdAtSec = *userReplyCreatedAt;
                out.push_back(user);
            }
            return true;
        });

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    std::thread workerThread([&]() {
        std::string werr;
        ctrl.RunSpawnSynchronouslyForTests(pid, werr);
    });

    // Wait for AwaitingUser — the controller will have stamped a cursor near
    // "now". We script the user reply createdAt to be strictly after the
    // cursor.
    bool sawAwaiting = false;
    for (int i = 0; i < 200 && !sawAwaiting; ++i) {
        const auto snap = ctrl.SnapshotAllForTests();
        if (!snap.empty() && snap[0].state == RunState::AwaitingUser) {
            sawAwaiting = true;
            *userReplyCreatedAt = snap[0].clarificationCommentCursorSec + 60; // 60 s after cursor
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(sawAwaiting);

    // First poll — only bot comments visible, no dispatch.
    int answered = ctrl.PollClarificationAnswers();
    CHECK(answered == 0);

    // Second poll — user reply now visible.
    answered = ctrl.PollClarificationAnswers();
    CHECK(answered == 1);

    workerThread.join();

    // The captured answer text matches the user reply (not the bot question).
    CHECK(runner.capturedAnswer == "yes, rename");

    RemoveFile(wt + "/CLARIFICATION_NEEDED.json");
    RemoveDir(wt);
}

TEST_CASE("AgenticHandoffController H5 — poll loop is a no-op when fetcher is unwired") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#780");
    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    AgenticHandoffController::ActiveHandoff h;
    std::string err;
    REQUIRE(ctrl.Start(pid, h, err));

    // No fetcher wired — poll loop reports 0 dispatches.
    CHECK(ctrl.PollClarificationAnswers() == 0);
}

// ─── H9 — cross-flow OnProposalApproved callback wiring ────────────────────
//
// The store fires `OnProposalApproved(id, action)` exactly on the
// Pending->Approved edge. Plan-locked decision #5: auto_start defaults FALSE
// — the callback decides whether to dispatch Start() based on a test-side
// gate that mirrors the production cfg `HandoffAutoStartOnApprove`. These
// tests assert four properties:
//   1. Non-ImplementIssue approvals never dispatch a handoff.
//   2. ImplementIssue approval with auto_start=false does NOT dispatch.
//   3. ImplementIssue approval with auto_start=true DOES dispatch.
//   4. Manual-button path (controller.Start invoked directly) appears in
//      SnapshotActive — covers the [Start handoff] panel flow that the
//      bucket-E test mirrors via stub controller.
//
// The four scenarios all use the same wiring pattern as AppController:
//   store.SetOnProposalApproved([...](id, action) {
//     if (action != ImplementIssue) return;
//     if (!autoStartGate) return;
//     ctrl.Start(id, ...);
//   });
// Tests bypass the worker dispatch by injecting an inline WorkerDispatcher
// so Start() drives the FakeRunner synchronously inside the callback.

TEST_CASE("AgenticHandoffController H9 — non-ImplementIssue approval does not start handoff") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertCommentAdd(store, "smatchet/example#h9-a");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    int callbackInvocations = 0;
    int startInvocations = 0;
    // auto_start=true on purpose — the callback should still no-op because
    // the action is CommentAdd, not ImplementIssue. Mirrors the production
    // AppController callback's first guard.
    const bool autoStart = true;
    store.SetOnProposalApproved([&](std::int64_t id, ProposedAction action) {
        ++callbackInvocations;
        if (action != ProposedAction::ImplementIssue) {
            return;
        }
        if (!autoStart) {
            return;
        }
        ++startInvocations;
        AgenticHandoffController::ActiveHandoff out;
        std::string serr;
        (void)ctrl.Start(id, out, serr);
    });

    std::string err;
    REQUIRE(store.Transition(pid, AgentProposalState::Approved, std::string(), err));
    CHECK(callbackInvocations == 1);
    CHECK(startInvocations == 0);
    CHECK(runner.spawnCalls == 0);
    CHECK(ctrl.SnapshotActive().empty());
}

TEST_CASE("AgenticHandoffController H9 — ImplementIssue approval + auto_start=false does NOT auto-start") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#h9-b");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    int callbackInvocations = 0;
    int startInvocations = 0;
    // The shipped default — decision #5. Approval still flips the row state
    // but the handoff waits for the user's button press.
    const bool autoStart = false;
    store.SetOnProposalApproved([&](std::int64_t id, ProposedAction action) {
        ++callbackInvocations;
        if (action != ProposedAction::ImplementIssue) {
            return;
        }
        if (!autoStart) {
            return;
        }
        ++startInvocations;
        AgenticHandoffController::ActiveHandoff out;
        std::string serr;
        (void)ctrl.Start(id, out, serr);
    });

    std::string err;
    REQUIRE(store.Transition(pid, AgentProposalState::Approved, std::string(), err));
    CHECK(callbackInvocations == 1);
    CHECK(startInvocations == 0);
    CHECK(runner.spawnCalls == 0);
    CHECK(ctrl.SnapshotActive().empty());
    CHECK(sink.Size() == 0); // no Start() => no audit transitions
}

TEST_CASE("AgenticHandoffController H9 — ImplementIssue approval + auto_start=true DOES auto-start") {
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#h9-c");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    int callbackInvocations = 0;
    int startInvocations = 0;
    const bool autoStart = true;
    store.SetOnProposalApproved([&](std::int64_t id, ProposedAction action) {
        ++callbackInvocations;
        if (action != ProposedAction::ImplementIssue) {
            return;
        }
        if (!autoStart) {
            return;
        }
        ++startInvocations;
        AgenticHandoffController::ActiveHandoff out;
        std::string serr;
        REQUIRE(ctrl.Start(id, out, serr));
    });

    std::string err;
    REQUIRE(store.Transition(pid, AgentProposalState::Approved, std::string(), err));
    CHECK(callbackInvocations == 1);
    CHECK(startInvocations == 1);

    // Start() bookkeeping landed: the handoff is registered with the
    // controller in Spawning state (Pending -> Spawning was the first FSM
    // transition the controller emitted).
    const auto active = ctrl.SnapshotActive();
    REQUIRE(active.size() == 1);
    CHECK(active[0].proposalId == pid);
    CHECK(active[0].issueKey == "smatchet/example#h9-c");
    // Spawning is the post-Start, pre-RunSpawnSynchronouslyForTests state.
    CHECK(active[0].state == RunState::Spawning);

    // Drive the worker so we can also assert the FakeRunner actually picked
    // up the seed (proves the cross-flow wiring reached spawn, not just the
    // controller bookkeeping).
    std::string drainErr;
    REQUIRE(ctrl.RunSpawnSynchronouslyForTests(pid, drainErr));
    CHECK(runner.spawnCalls == 1);
    CHECK(runner.capturedSeed.proposalId == pid);
}

TEST_CASE("AgenticHandoffController H9 — manual button path: direct Start appears in SnapshotActive") {
    // Mirrors the SmatchetAgentProposalsUi `[Start handoff]` button path:
    // panel calls store.Transition(Approved) (auto-start callback is a no-op
    // in manual mode) then calls ctrl.Start() directly. SnapshotActive
    // surfaces the in-flight handoff so the H8 handoff panel can render it.
    AgentProposalStore store(":memory:");
    const std::int64_t pid = InsertImplementIssue(store, "smatchet/example#h9-d");

    FakeRunner runner(FakeRunner::Mode::HappyPath);
    CapturingSink sink;
    AgenticHandoffController ctrl(::smatchet::agentic::WorkerDispatcher(), &runner, &store, MakeCapturingSink(sink));

    // Manual mode — callback is wired (mirroring AppController) but the
    // auto_start gate is false, so it intentionally no-ops on the approval.
    int callbackInvocations = 0;
    int autoStartInvocations = 0;
    const bool autoStart = false;
    store.SetOnProposalApproved([&](std::int64_t id, ProposedAction action) {
        ++callbackInvocations;
        if (action != ProposedAction::ImplementIssue) {
            return;
        }
        if (!autoStart) {
            return;
        }
        ++autoStartInvocations;
        AgenticHandoffController::ActiveHandoff out;
        std::string serr;
        (void)ctrl.Start(id, out, serr);
    });

    std::string err;
    REQUIRE(store.Transition(pid, AgentProposalState::Approved, std::string(), err));
    CHECK(callbackInvocations == 1);
    CHECK(autoStartInvocations == 0);
    CHECK(ctrl.SnapshotActive().empty());

    // Now the panel's button-press path: invoke Start() explicitly. The
    // approve-and-start panel helper is the production analogue.
    AgenticHandoffController::ActiveHandoff manual;
    std::string startErr;
    REQUIRE(ctrl.Start(pid, manual, startErr));
    CHECK(manual.proposalId == pid);

    const auto active = ctrl.SnapshotActive();
    REQUIRE(active.size() == 1);
    CHECK(active[0].proposalId == pid);
    CHECK(active[0].state == RunState::Spawning);
}

#endif // SMATCHET_WITH_AGENTIC
