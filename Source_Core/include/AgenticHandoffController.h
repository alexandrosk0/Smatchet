#ifndef SMATCHET_AGENTIC_HANDOFF_CONTROLLER_H
#define SMATCHET_AGENTIC_HANDOFF_CONTROLLER_H

// AgenticHandoffController — the orchestrator class that drives one
// HarnessRunState lifecycle end-to-end for an approved ImplementIssue
// proposal. H4 ships the in-memory handoff record + the worker-thread
// dispatch; H10 will persist the row to `agent_pr_watch` so an app
// restart can rejoin the watch loop.
//
// Threading contract (pillar 2 — UI never freezes):
//   - Start / ProvideClarification / Cancel are synchronous *and only do
//     bookkeeping* — they never call Runner::Spawn / Resume on the calling
//     thread. The runner work is posted via AppController::LaunchBackgroundTask
//     onto a worker thread; state-transition callbacks marshal back to the
//     UI thread via MainThreadDispatcher::PostToMainThread.
//   - SnapshotActive() is safe to call from any thread; it copies under the
//     internal mutex.
//
// FSM enforcement:
//   - The runner emits state-name strings; ControllerTransition() validates
//     each one through HarnessRunState::IsTransitionAllowed before
//     audit-trailing + storing. Disallowed transitions LOG_WARN and are
//     dropped — the runner is well-behaved but the integrity boundary lives
//     here (AGENTS.md § Anti-deception note).
//
// Build-time gating: source-list-conditional in the root CMakeLists.txt on
// SMATCHET_WITH_AGENTIC. The header consumes ICodingHarnessRunner +
// AgentProposalStore + AgenticInferenceClientPure all of which live behind
// the gate; the entire header body is gated.

#include "HarnessRunState.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposal.h"           // AgentProposal
#include "CodingHarnessTypes.h"      // Seed / ClarificationResponse / RunResult

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class AgentProposalStore;

namespace CodingHarness {
class IRunner;
}

namespace smatchet {
namespace agentic {

// Injectable audit sink. Production wires this to
// `BackendAuditTrail::AppendEvent`; the doctest rig captures entries to a
// vector for assertion. The free-function namespace API of BackendAuditTrail
// is not virtual, so a sink lambda is the cleanest seam.
using AuditSink = std::function<void(const std::string& action, const std::string& source,
                                     const std::string& issueKey, bool success,
                                     const std::string& errorMessage, const nlohmann::json& data)>;

// Worker dispatcher seam — production binds to
// `AppController::LaunchBackgroundTask`; tests pass an empty function and
// drive the worker through `RunSpawnSynchronouslyForTests`. Type-erasing the
// dispatch keeps `AgenticHandoffController.cpp` free of an AppController
// symbol dependency so the test exe can link the controller TU without
// pulling AppController.cpp's whole transitive include set.
using WorkerDispatcher = std::function<void(std::function<void()>)>;

class AgenticHandoffController {
  public:
    // One in-flight handoff record. Lives in-memory only for H4; H10 will
    // persist mirroring fields to SQLite (agent_pr_watch table).
    struct ActiveHandoff {
        std::int64_t proposalId = 0;
        std::string worktreeDir;     // absolute path
        std::string branchName;      // `agent/<proposalId>/<short-slug>`
        CodingHarness::RunState state = CodingHarness::RunState::Pending;
        std::string lastError;       // populated when state is Failed
        std::string prUrl;           // populated when state reaches PrOpen
        std::int64_t startedAtSec = 0;
        std::shared_ptr<std::atomic<bool>> cancelToken;
    };

    // Non-owning pointers — AppController owns `runner` / `proposalStore`
    // and outlives this controller per the standard Smatchet lifetime
    // contract.
    //
    // `dispatcher` is the worker-thread dispatch seam (production binds to
    // `AppController::LaunchBackgroundTask`). Pass an empty function and
    // drive Spawn() inline through `RunSpawnSynchronouslyForTests` from
    // tests.
    //
    // `auditSink` may be null — the controller falls back to a no-op sink
    // in that case so tests do not have to wire BackendAuditTrail.
    AgenticHandoffController(WorkerDispatcher dispatcher, CodingHarness::IRunner* runner,
                             AgentProposalStore* proposalStore, AuditSink auditSink);
    ~AgenticHandoffController();

    AgenticHandoffController(const AgenticHandoffController&) = delete;
    AgenticHandoffController& operator=(const AgenticHandoffController&) = delete;

    // Begins a handoff for the given proposal. Pre-conditions:
    //   - The proposal exists in the store.
    //   - Its `action == ImplementIssue` (the FSM only governs implementation
    //     runs; triage-only actions stay in the proposal store).
    //   - The proposal is not already in flight (one record per id at a time).
    //
    // On success: returns true, populates `outHandoff` with the new record
    // (in Spawning state — the worker has been dispatched), and the worker
    // drives the rest of the lifecycle.
    //
    // On precondition failure: returns false, leaves `outHandoff` empty,
    // writes a short cause to `outError`. No audit entry is recorded for
    // precondition failures — those are caller-side validation, not run
    // events.
    bool Start(std::int64_t proposalId, ActiveHandoff& outHandoff, std::string& outError);

    // Drops `USER_RESPONSE.json` into the named handoff's worktree by
    // posting `runner.Resume()` onto a worker thread. Returns false +
    // outError if the handoff is unknown / not in AwaitingUser state.
    bool ProvideClarification(std::int64_t proposalId, const std::string& answer, std::string& outError);

    // Flips the cancel atom; expects the runner to honour it within a
    // handful of seconds and surface Cancelled via its state callback. The
    // FSM rejects Cancel on a terminal state (caller never sees a state
    // change in that branch).
    bool Cancel(std::int64_t proposalId, std::string& outError);

    // Snapshot of all in-flight (non-terminal) handoffs. Cheap deep-copy
    // under the internal mutex; safe from any thread. The H8 UI panel reads
    // via this method on every frame.
    std::vector<ActiveHandoff> SnapshotActive() const;

    // Test-only — returns the full table including terminal entries. Tests
    // assert on Cancelled / Failed / Complete records that SnapshotActive
    // would otherwise filter. Not exposed via CLI commands.
    std::vector<ActiveHandoff> SnapshotAllForTests() const;

    // Test-only synchronous driver. Production callers go through Start();
    // tests bypass the worker dispatch by calling this directly so the
    // doctest stays single-threaded + deterministic. Returns the final
    // state of the handoff after `runner.Spawn` returns.
    bool RunSpawnSynchronouslyForTests(std::int64_t proposalId, std::string& outError);

    // Builds the short slug for an issue key per plan-locked decision #1
    // ("agent/<proposalId>/<short-slug>" + 32-char cap). Exposed so the
    // CLI `handoff.start --dry-run` can preview the branch name without
    // mutating state.
    static std::string BuildShortSlug(const std::string& issueKey);

    // Builds the canonical branch name. The 32-char cap matches the
    // BuildShortSlug guarantee. Branch is fed verbatim to
    // `git worktree add -b`.
    static std::string BuildBranchName(std::int64_t proposalId, const std::string& issueKey);

    // Builds the canonical worktree dir per plan decision #2:
    //   `.claude/worktrees/agent-<proposalId>` (relative to repo root).
    static std::string BuildWorktreeDir(std::int64_t proposalId);

  private:
    // Single source of truth for FSM transitions. Validates `from -> to`
    // against `HarnessRunState::IsTransitionAllowed`, emits an audit entry,
    // updates the in-memory record. Returns true on success; false +
    // LOG_WARN on disallowed transition (the runner emitted a bad state).
    bool ControllerTransition(std::int64_t proposalId, CodingHarness::RunState toState,
                              const std::string& errorMessage, const std::string& prUrl);

    // Worker entry point. Performs:
    //   Pending -> Spawning (audit)
    //   runner_->Spawn(...) with callbacks that funnel into
    //   ControllerTransition. Each callback marshals to the UI thread via
    //   MainThreadDispatcher when `app_` is non-null; tests pass null and
    //   the callback runs on the worker.
    //   On runner return: Complete / Failed terminal transition.
    void RunHandoffWorker(std::int64_t proposalId);

    // Helper: emits an audit entry with the standard payload shape. The
    // payload always includes `{proposalId, fromState, toState}` plus any
    // additional fields (prUrl, errorMessage) the caller supplies.
    void EmitAudit(const std::string& issueKey, CodingHarness::RunState fromState,
                   CodingHarness::RunState toState, bool success, const std::string& errorMessage,
                   const std::string& prUrl);

    WorkerDispatcher dispatcher_;
    CodingHarness::IRunner* runner_;
    AgentProposalStore* proposalStore_;
    AuditSink auditSink_;

    mutable std::mutex handoffsMu_;
    std::unordered_map<std::int64_t, ActiveHandoff> handoffs_;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC

#endif // SMATCHET_AGENTIC_HANDOFF_CONTROLLER_H
