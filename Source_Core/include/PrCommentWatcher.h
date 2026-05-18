// PrCommentWatcher.h — H7 PR-thread watcher for in-flight handoffs.
//
// Once a handoff transitions to PrOpen the runner is done writing code, but
// the operator (or PR-reviewing bots) may post comments requesting changes.
// `PrCommentWatcher` polls the PR's comment thread for non-bot replies and
// respawns the harness against the same branch + worktree so the spawned
// `claude` (via the `pr-iterator` agent) can iterate on the feedback.
//
// Design — piggybacks on the existing scheduled-poll worker. The `Tick()`
// method is meant to be called once per poll iteration; the watcher owns no
// thread of its own. Same shape as H5's `PollClarificationAnswers` — a
// single 4th poll thread would add cost (sleep, cancel, restart) without
// changing the wake cadence. This is the documented choice in
// docs/design/agentic-flow-implementation.md § H7.
//
// Iteration budget — every dispatched respawn increments `iterationCount`
// on the handoff record. Once `iterationCount >= budget` the watcher posts
// a final "budget exhausted" comment to the PR (bot-filtered marker) and
// transitions the handoff to Failed. Subsequent ticks skip the handoff so
// the comment is never posted twice nor the state flipped more than once.
//
// Bot-filter — comments whose body matches
// `AgenticHandoffController::IsHandoffBotComment` are skipped, which uses
// the SAME `<!-- smatchet-handoff -->` marker that the H5 issue-comment
// path posts. This is critical: the watcher's own "budget exhausted"
// comment carries the marker so the next tick does not mistake it for a
// user reply.
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC.

#ifndef SMATCHET_PR_COMMENT_WATCHER_H
#define SMATCHET_PR_COMMENT_WATCHER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgenticHandoffController.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class AgentProposalStore;

namespace smatchet {
namespace agentic {

// PR-comment fetcher seam — production binds to a lambda calling
// `GitHubClient::FetchPrComments`. Returns false + outError on transport
// failure; the watcher logs + skips that iteration. The `prKey` here uses
// the canonical `owner/repo#N` shape (same as the issue-comment fetcher);
// the watcher derives it from the `prUrl` field on the handoff record.
//
// Tests inject a fake that scripts the comment list per call.
using PrCommentFetcher = std::function<bool(const std::string& /*prKey*/,
                                            std::vector<PostedComment>& /*outComments*/,
                                            std::string& /*outError*/)>;

// PR-comment poster seam — production binds to `GitHubClient::CommentAdd`
// (PR comments and issue comments share the same REST endpoint on GitHub).
// Used by the watcher to post the "budget exhausted" marker comment.
// Failure is logged + non-fatal — the state transition still happens.
using PrCommentPoster = std::function<bool(const std::string& /*prKey*/, const std::string& /*body*/,
                                           std::string& /*outError*/)>;

// Handoff-respawn dispatcher seam — invoked when the watcher detects a new
// non-bot PR comment that the spawned harness should react to. Production
// re-invokes `AgenticHandoffController::Start` against the same proposal
// id (the seed builder picks up the latest PR comment via the same
// PR_URL.txt sentinel); tests capture (proposalId, commentBody) pairs.
// Returns false + outError when the respawn cannot proceed (worktree
// missing, runner unavailable); the watcher LOG_WARNs and continues.
using HarnessRespawnDispatcher = std::function<bool(std::int64_t /*proposalId*/, const std::string& /*commentBody*/,
                                                    std::string& /*outError*/)>;

class PrCommentWatcher {
  public:
    // `controller` is non-owning — production passes the AppController's
    // handoff-controller pointer. Lifetime is bounded by AppController.
    // `iterationBudget` is the per-handoff cap; clamped to [1, 50] in the
    // ctor to match the ConfigManager clamp. Default 10 mirrors
    // `cfg.HandoffPrIterationBudget`.
    PrCommentWatcher(AgenticHandoffController* controller, int iterationBudget = 10);
    ~PrCommentWatcher();

    PrCommentWatcher(const PrCommentWatcher&) = delete;
    PrCommentWatcher& operator=(const PrCommentWatcher&) = delete;

    /// Wire the PR-comment-fetcher seam. Optional — when null the watcher
    /// is a no-op (every Tick returns 0 + LOG_DEBUGs once). Setter is not
    /// thread-safe relative to in-flight ticks; wire at AppController
    /// init before the scheduled-poll worker spawns.
    void SetPrCommentFetcher(PrCommentFetcher fetcher);
    /// Wire the PR-comment-poster seam used for the budget-exhausted marker.
    /// Optional — when null the watcher still flips the state to Failed
    /// but cannot post the marker (the operator must look at the audit
    /// trail).
    void SetPrCommentPoster(PrCommentPoster poster);
    /// Wire the harness-respawn dispatcher. Optional — when null Tick()
    /// LOG_WARNs once per detected comment and skips the dispatch (the
    /// handoff stays in PrOpen, the operator must respawn manually).
    void SetRespawnDispatcher(HarnessRespawnDispatcher dispatcher);

    /// Per-handoff iteration cap. Setter clamps to [1, 50].
    void SetIterationBudget(int budget);
    int GetIterationBudget() const;

    /// (H10) Wire the SQLite cursor-persistence seam. Optional — when null
    /// the watcher operates in-memory only (H7 behaviour, lost across
    /// restarts). When wired:
    ///   - Tick() persists the (proposalId, prUrl, last_seen_comment_id_str,
    ///     iteration_count, last_polled_at_sec) row to `agent_pr_watch`
    ///     after every successful poll.
    ///   - `LoadCursorsFromStore` hydrates the controller's in-memory
    ///     ActiveHandoff records from any persisted rows on startup.
    /// Setter is not thread-safe relative to in-flight ticks; wire at
    /// AppController init before the scheduled-poll worker spawns.
    void SetProposalStore(AgentProposalStore* store);

    /// (H10) Hydrate the controller's in-memory ActiveHandoff records from
    /// `agent_pr_watch` rows. Called at AppController init after the
    /// controller is constructed but before the poll worker starts. For
    /// each persisted row whose proposal id corresponds to a currently-
    /// PrOpen handoff, copies `lastSeenCommentIdStr` + `iterationCount`
    /// onto the matching ActiveHandoff record (the controller's
    /// `MarkHandoffIteration` is reused so the FSM contract holds). Rows
    /// with no matching handoff (handoff terminated between sessions) are
    /// best-effort cleaned up by the next `DeletePrWatch` call from the
    /// controller termination path. Returns the number of cursors loaded
    /// (zero is a valid steady-state).
    int LoadCursorsFromStore();

    /// One iteration of the watcher. Snapshots all PrOpen handoffs, fetches
    /// each PR's comments, dispatches a respawn for any new non-bot
    /// comment (or trips the budget-exhausted path). Returns the number
    /// of respawns dispatched + budget-exhausted transitions combined.
    ///
    /// Safe to call from any worker thread; takes the controller's mutex
    /// via its existing snapshot methods. Never touches ImGui state.
    int Tick();

    /// Build the body for the "budget exhausted" comment posted to the PR
    /// when iterationCount hits the budget. Carries the bot-filter marker
    /// so the next Tick skips it. Exposed for tests.
    static std::string BuildBudgetExhaustedCommentBody(std::int64_t proposalId, int iterationsUsed);

    /// Derive the canonical `owner/repo#N` PR key from a PR URL like
    /// `https://github.com/owner/repo/pull/42`. Returns false + outError
    /// on a malformed URL (missing /pull/, non-numeric N). Exposed for
    /// tests; the watcher uses it internally on every tick.
    static bool ParsePrKeyFromUrl(const std::string& prUrl, std::string& outPrKey, std::string& outError);

  private:
    AgenticHandoffController* controller_;
    PrCommentFetcher fetcher_;
    PrCommentPoster poster_;
    HarnessRespawnDispatcher dispatcher_;
    std::atomic<int> iterationBudget_;
    // (H7) Latch so the "fetcher is null" log fires once per session
    // instead of every tick — same shape as the H5 dual-channel.
    std::atomic<bool> fetcherWarnLatched_{false};
    std::atomic<bool> dispatcherWarnLatched_{false};
    // (H10) SQLite cursor-persistence seam. Optional — null falls back to
    // H7 in-memory-only behaviour. Lifetime owned by AppController.
    AgentProposalStore* store_ = nullptr;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC

#endif // SMATCHET_PR_COMMENT_WATCHER_H
