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

#include "AgentProposal.h"      // AgentProposal
#include "CodingHarnessTypes.h" // Seed / ClarificationResponse / RunResult

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
using AuditSink = std::function<void(const std::string& action, const std::string& source, const std::string& issueKey,
                                     bool success, const std::string& errorMessage, const nlohmann::json& data)>;

// Worker dispatcher seam — production binds to
// `AppController::LaunchBackgroundTask`; tests pass an empty function and
// drive the worker through `RunSpawnSynchronouslyForTests`. Type-erasing the
// dispatch keeps `AgenticHandoffController.cpp` free of an AppController
// symbol dependency so the test exe can link the controller TU without
// pulling AppController.cpp's whole transitive include set.
using WorkerDispatcher = std::function<void(std::function<void()>)>;

// GitHub-comment poster seam (H5) — production binds to a lambda calling
// `GitHubClient::CommentAdd`; tests inject a fake that captures (issueKey,
// body) pairs. The controller never depends on `GitHubClient` directly so
// the test TU does not need to link cpr / the full GitHub stack. The seam
// returns false + outError on transport failure; the controller LOG_WARNs
// and continues — the worktree-file channel is the canonical path, the
// GitHub comment is an audit-trail mirror that must not fail-stop the run.
using GitHubCommentPoster =
    std::function<bool(const std::string& /*issueKey*/, const std::string& /*body*/, std::string& /*outError*/)>;

// GitHub-comment fetcher seam (H5) — production binds to a lambda calling
// `GitHubClient::FetchIssueComments`. The piggyback poll worker uses this
// to look for non-bot answers on in-flight AwaitingUser handoffs. Returns
// false + outError on transport failure; the worker logs + skips that
// iteration. Tests inject a fake that scripts the comment list per call.
struct PostedComment {
    std::string id;     // backend-stable comment id (for de-dupe)
    std::string author; // user.login
    std::string body;   // raw markdown
    std::int64_t createdAtSec = 0;
};
using GitHubCommentFetcher = std::function<bool(
    const std::string& /*issueKey*/, std::vector<PostedComment>& /*outComments*/, std::string& /*outError*/)>;

// (H6) Toast sink seam — production binds to a lambda calling
// `SmatchetToastManager::Instance().Push(...)`. Decoupled via a function-typed
// seam so the controller TU does not pull `SmatchetToast.cpp` (which depends
// on ImGui) into the test rig. Null = no-op (the doctest path).
//
// The controller invokes the sink once per PrOpen transition with a short
// human-readable string carrying the PR URL. Future callers may extend the
// arity (toast type, body, duration) — keep this minimal until a real
// requirement appears.
using ToastSink = std::function<void(const std::string& /*message*/)>;

class AgenticHandoffController {
  public:
    // One in-flight handoff record. Lives in-memory only for H4; H10 will
    // persist mirroring fields to SQLite (agent_pr_watch table).
    struct ActiveHandoff {
        std::int64_t proposalId = 0;
        std::string worktreeDir; // absolute path
        std::string branchName;  // `agent/<proposalId>/<short-slug>`
        std::string issueKey;    // mirror of AgentProposal.issueKey — populated on Start so
                                 // the H5 GitHub-comment path + H8 panel don't have to re-fetch
                                 // the proposal row from the store on every read.
        CodingHarness::RunState state = CodingHarness::RunState::Pending;
        std::string lastError; // populated when state is Failed
        std::string prUrl;     // populated when state reaches PrOpen
        std::int64_t startedAtSec = 0;
        std::shared_ptr<std::atomic<bool>> cancelToken;

        // (H5) Most recent clarification question observed by the runner —
        // populated when the controller reads CLARIFICATION_NEEDED.json from
        // the worktree on the AwaitingUser transition. The H8 UI panel reads
        // this field on every frame to surface the prompt; cleared back to
        // empty when ProvideClarification succeeds. Empty when the handoff
        // has never been in AwaitingUser, or after the user replies.
        std::string lastClarificationQuestion;
        std::int64_t lastClarificationAtSec = 0;
        // (H5) Bookkeeping for the piggyback poll worker — when the
        // GitHub-comment poll loop scans this handoff's issue for non-bot
        // replies, it advances this cursor on each iteration so the next
        // iteration only inspects comments created strictly after the
        // baseline. Defaults to startedAtSec on the AwaitingUser transition
        // so the question we just posted is not mistaken for the answer.
        std::int64_t clarificationCommentCursorSec = 0;
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

    // (H5) Wire the GitHub-comment dual-channel seams. Both are optional —
    // when either is null the controller operates worktree-only and writes
    // a LOG_INFO at first opportunity so the operator knows. Call after
    // construction; the setters are not thread-safe relative to in-flight
    // handoffs (callers wire at AppController init before any Start).
    void SetGitHubCommentPoster(GitHubCommentPoster poster);
    void SetGitHubCommentFetcher(GitHubCommentFetcher fetcher);

    // (H5) Toggle that mirrors `cfg.HandoffClarificationPostToGithub`.
    // When false the poster is never invoked even if SetGitHubCommentPoster
    // was wired — useful for private / non-GitHub work where the operator
    // wants the audit trail purely local. Defaults to true.
    void SetGitHubClarificationEnabled(bool enabled);

    // (H6) Wire the toast sink. Optional — null leaves the controller silent
    // on PrOpen (the audit-trail entry still records the URL). Setter is not
    // thread-safe relative to in-flight handoffs; wire at AppController init
    // before any Start.
    void SetToastSink(ToastSink sink);

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

    // ─── H5 bot-filter / formatting helpers ────────────────────────────────
    //
    // The HTML-comment marker `<!-- smatchet-handoff -->` is the bot-filter
    // boundary — every comment Smatchet posts to a GitHub issue starts with
    // this literal, and the poll loop discards any comment whose body starts
    // with the same literal so we never treat our own posted question as a
    // user reply. The same marker is reused by H7's `PrCommentWatcher` for
    // PR-thread comments — keep the literal here so both watchers reference
    // one source of truth. CRITICAL: changing the literal breaks every
    // in-flight handoff's poll loop until the next start; flag in a backlog
    // entry first.

    /// The exact marker prefix used to fingerprint bot-authored comments.
    /// Callers compare via `IsHandoffBotComment` rather than substring-match
    /// directly so the comparison rules (whitespace tolerance, case) stay in
    /// one place.
    static const char* HandoffBotMarker();

    /// True when the comment body begins with the marker (modulo leading
    /// whitespace). Case-sensitive — the marker is literal HTML and GitHub
    /// preserves its case verbatim.
    static bool IsHandoffBotComment(const std::string& body);

    /// Build the comment body posted when the runner emits AwaitingUser.
    /// Shape:
    ///   <!-- smatchet-handoff -->
    ///
    ///   **Agent question (proposal #<id>):**
    ///
    ///   {question}
    ///
    ///   Reply to this comment to answer.
    static std::string BuildClarificationCommentBody(std::int64_t proposalId, const std::string& question);

    /// Build the comment body posted on `ProvideClarification` so the GitHub
    /// thread has an audit trail of what the user answered.
    /// Shape:
    ///   <!-- smatchet-handoff -->
    ///
    ///   **User answered (proposal #<id>):**
    ///
    ///   {answer}
    static std::string BuildAnswerCommentBody(std::int64_t proposalId, const std::string& answer);

    // ─── H5 poll-loop driver ─────────────────────────────────────────────
    //
    // Piggybacked on T7's existing scheduled-poll worker — every iteration
    // the worker calls `PollClarificationAnswers()` which scans in-flight
    // AwaitingUser handoffs and dispatches `ProvideClarification` when the
    // GitHub-comment fetcher returns a non-bot comment newer than the
    // handoff's clarificationCommentCursorSec. Returns the count of answers
    // dispatched so the worker can LOG_INFO useful telemetry.
    //
    // Safe to call from any worker thread; takes the controller's mutex to
    // snapshot AwaitingUser handoffs before walking each. Never touches
    // ImGui state.
    int PollClarificationAnswers();

  private:
    // Single source of truth for FSM transitions. Validates `from -> to`
    // against `HarnessRunState::IsTransitionAllowed`, emits an audit entry,
    // updates the in-memory record. Returns true on success; false +
    // LOG_WARN on disallowed transition (the runner emitted a bad state).
    bool ControllerTransition(std::int64_t proposalId, CodingHarness::RunState toState, const std::string& errorMessage,
                              const std::string& prUrl);

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
    void EmitAudit(const std::string& issueKey, CodingHarness::RunState fromState, CodingHarness::RunState toState,
                   bool success, const std::string& errorMessage, const std::string& prUrl);

    // (H5) Parses CLARIFICATION_NEEDED.json from the named worktree and
    // stashes the question on the handoff record. Called from the runner's
    // state-change callback when toState == AwaitingUser. Best-effort: a
    // missing or malformed file LOG_WARNs but leaves the field empty — the
    // FSM transition still records normally so the H8 UI panel surfaces
    // "AwaitingUser (no question text)" rather than blocking the state.
    void ReadAndStashClarificationQuestion(std::int64_t proposalId);

    // (H5) Posts the bot-filtered comment to the GitHub issue when both the
    // poster seam is wired and the clarification-via-GitHub toggle is on.
    // Failure to post is non-fatal — LOG_WARNs and returns false; the
    // controller still operates worktree-only. Runs on whichever thread
    // invokes the state callback (worker thread under production, the test
    // thread under doctest).
    bool PostClarificationToGitHub(const std::string& issueKey, std::int64_t proposalId, const std::string& question);
    bool PostAnswerToGitHub(const std::string& issueKey, std::int64_t proposalId, const std::string& answer);

    WorkerDispatcher dispatcher_;
    CodingHarness::IRunner* runner_;
    AgentProposalStore* proposalStore_;
    AuditSink auditSink_;

    // (H5) Wired post-construction. nullptr = local-only fallback.
    GitHubCommentPoster githubPoster_;
    GitHubCommentFetcher githubFetcher_;
    std::atomic<bool> githubClarificationEnabled_{true};

    // (H6) Optional toast sink. nullptr = silent (audit-trail only).
    ToastSink toastSink_;

    mutable std::mutex handoffsMu_;
    std::unordered_map<std::int64_t, ActiveHandoff> handoffs_;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC

#endif // SMATCHET_AGENTIC_HANDOFF_CONTROLLER_H
