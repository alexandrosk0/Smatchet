// PrCheckRunWatcher.h — phase-6 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md § Phased rollout § Phase 6).
// Sibling to `PrCommentWatcher` (`WatchMode::OpenPrScan` mode); polls
// `agent_open_pr_watch` rows for newly-failed check-runs and routes
// each through the injected `PrCheckRunClassifier` (production: the
// concrete `CiFailureClassifier`).
//
// Tick()-only — no internal thread. Same shape as `PrCommentWatcher`
// per the 2026-05-18 locked decision (one shared scheduled-poll worker
// drives the comment watcher → registrar → check-run watcher chain).
//
// Verdict branches (mirror of PrCommentWatcher's open-PR-scan loop):
//   - Skip            → log + advance cursor (so the same check-run is
//                       not re-classified next tick).
//   - RerunTransient  → call the `WorkflowRerunDispatcher` seam (production
//                       binds to `GitHubClient::RerunWorkflowRun`); count
//                       per-PR rerun against `transientRerunCap_`. Cursor
//                       advances + iteration_count does NOT bump (a rerun
//                       is not a "respawn").
//   - Dispatch        → call the `CheckRunRespawnDispatcher` seam (phase-7
//                       binding; phase-6 watcher LOG_WARNs once when
//                       null). Cursor advances + iteration_count bumps.
//
// Per-PR iteration budget (`iterationBudget_`) caps the CheckRunRespawnDispatcher
// firings against any one PR — same as PrCommentWatcher. Exceeding the
// budget logs + skips (no "budget exhausted" comment is posted for check-run
// dispatches; comment-mode handles the user-facing notice).
//
// Smatchet-own-marker skip path: present but no-op today — Smatchet does
// not author check-runs. Kept as a hook so a future Smatchet-CI integration
// can opt out trivially.
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC.

#ifndef SMATCHET_PR_CHECK_RUN_WATCHER_H
#define SMATCHET_PR_CHECK_RUN_WATCHER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "GitHubClient.h" // for GitHubClient::CheckRun + CheckRunAnnotation

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class AgentProposalStore;

namespace smatchet {
namespace agentic {

class PrCheckRunClassifier;

// CheckRun fetcher seam — production binds to a lambda calling
// `GitHubClient::FetchCheckRuns(owner, repo, headSha)`. The watcher derives
// `(owner, repo)` from the `agent_open_pr_watch.pr_url` field and passes
// `head_sha` from the row directly.
using CheckRunFetcher =
    std::function<bool(const std::string& /*owner*/, const std::string& /*repo*/, const std::string& /*headSha*/,
                       std::vector<GitHubClient::CheckRun>& /*outRuns*/, std::string& /*outError*/)>;

// CheckRun annotation fetcher seam — production binds to
// `GitHubClient::FetchCheckRunAnnotations(owner, repo, checkRunId)`.
using CheckRunAnnotationFetcher =
    std::function<bool(const std::string& /*owner*/, const std::string& /*repo*/, std::int64_t /*checkRunId*/,
                       std::vector<GitHubClient::CheckRunAnnotation>& /*outAnnotations*/, std::string& /*outError*/)>;

// Log-tail fetcher seam — phase-6 leaves this optional. When null the
// classifier sees an empty log tail and works off annotations alone.
// Production binding (when wired) resolves the Actions job-id from the
// check-run's details_url and calls `GitHubClient::FetchActionsJobLogs`.
using LogTailFetcher =
    std::function<bool(const std::string& /*owner*/, const std::string& /*repo*/, const std::string& /*detailsUrl*/,
                       int /*tailLines*/, std::string& /*outLogTail*/, std::string& /*outError*/)>;

// Workflow rerun dispatcher seam — production binds to
// `GitHubClient::RerunWorkflowRun(owner, repo, runId)`. Returns false +
// outError on transport failure; the watcher logs + skips the rerun for
// this tick (the cursor still advances so the same check-run is not
// reprocessed).
using WorkflowRerunDispatcher = std::function<bool(const std::string& /*owner*/, const std::string& /*repo*/,
                                                   std::int64_t /*runId*/, std::string& /*outError*/)>;

// Open-PR-respawn dispatcher — mirror of PrCommentWatcher's seam of the
// same name. Phase-7 binds to a worktree-spawning dispatcher; phase-6
// leaves null on purpose so a real CI failure cannot trigger an unintended
// spawn before the worktree-management code lands. Returns false +
// outError on dispatch failure; the watcher LOG_WARNs and skips this run.
using CheckRunRespawnDispatcher = std::function<bool(
    const std::string& /*prUrl*/, const std::string& /*dispatchSource*/, const std::string& /*dispatchTargetAgent*/,
    const GitHubClient::CheckRun& /*run*/, std::string& /*outError*/)>;

class PrCheckRunWatcher {
  public:
    // `store` is non-owning. `iterationBudget` is the per-PR cap on the
    // CheckRunRespawnDispatcher; clamped to [1, 50]. `transientRerunCap` is
    // the per-PR cap on `WorkflowRerunDispatcher`; clamped to [0, 10]
    // (0 disables the rerun path entirely).
    explicit PrCheckRunWatcher(AgentProposalStore* store, int iterationBudget = 5, int transientRerunCap = 2);
    ~PrCheckRunWatcher();

    PrCheckRunWatcher(const PrCheckRunWatcher&) = delete;
    PrCheckRunWatcher& operator=(const PrCheckRunWatcher&) = delete;

    /// Wire the check-run fetcher seam. Optional — null = no-op Tick.
    void SetCheckRunFetcher(CheckRunFetcher fetcher);
    /// Wire the annotation fetcher seam. Optional — null = classifier
    /// sees an empty annotation list (still fingerprints log-tail).
    void SetAnnotationFetcher(CheckRunAnnotationFetcher fetcher);
    /// Wire the log-tail fetcher seam. Optional — null = classifier sees
    /// an empty log tail.
    void SetLogTailFetcher(LogTailFetcher fetcher);
    /// Wire the workflow-rerun dispatcher. Optional — null = the watcher
    /// LOG_WARNs once on RerunTransient verdicts but advances the cursor.
    void SetWorkflowRerunDispatcher(WorkflowRerunDispatcher dispatcher);
    /// Wire the open-PR respawn dispatcher. Optional — phase-6 leaves
    /// this null on purpose; phase-7 binds it.
    void SetCheckRunRespawnDispatcher(CheckRunRespawnDispatcher dispatcher);

    /// Register the classifier. Heap-owned (polymorphic). Passing nullptr
    /// resets to "no classifier" — Tick() advances cursors but never
    /// dispatches.
    void SetClassifier(std::unique_ptr<PrCheckRunClassifier> classifier);

    /// Per-PR iteration cap setters. Clamps as above.
    void SetIterationBudget(int budget);
    int GetIterationBudget() const;
    void SetTransientRerunCap(int cap);
    int GetTransientRerunCap() const;

    /// One iteration of the watcher. Iterates every row in
    /// `agent_open_pr_watch`, fetches failed check-runs, classifies each
    /// newer-than-cursor entry, applies the verdict. Returns the number
    /// of dispatches + reruns issued combined. Safe to call from any
    /// worker thread; never touches ImGui state.
    int Tick();

    /// Read-only — per-PR rerun counter. Exposed for tests + audit.
    int GetRerunCount(const std::string& prUrl) const;

    /// Derive `(owner, repo)` from a canonical GitHub PR URL like
    /// `https://github.com/owner/repo/pull/42`. Returns false + outError
    /// on malformed input. Mirrors PrCommentWatcher::ParsePrKeyFromUrl
    /// but returns the two segments separately because the check-run
    /// REST endpoint takes `(owner, repo)` not `(owner/repo#N)`.
    static bool ParseOwnerRepoFromPrUrl(const std::string& prUrl, std::string& outOwner, std::string& outRepo,
                                        std::string& outError);

  private:
    AgentProposalStore* store_;
    CheckRunFetcher checkRunFetcher_;
    CheckRunAnnotationFetcher annotationFetcher_;
    LogTailFetcher logTailFetcher_;
    WorkflowRerunDispatcher rerunDispatcher_;
    CheckRunRespawnDispatcher respawnDispatcher_;
    std::unique_ptr<PrCheckRunClassifier> classifier_;
    std::atomic<int> iterationBudget_;
    std::atomic<int> transientRerunCap_;
    // Latches so the "not wired" log fires once per session, not every tick.
    std::atomic<bool> fetcherWarnLatched_{false};
    std::atomic<bool> respawnDispatcherWarnLatched_{false};
    std::atomic<bool> rerunDispatcherWarnLatched_{false};
    // Per-PR rerun counter — in-memory only, lost on restart by design.
    // A persistent transient flake surfaces as a small re-count post-restart,
    // which is louder than silently exceeding the cap forever.
    //
    // Guarded by `rerunCountMu_` — `Tick()` claims it briefly to bump the
    // counter, `GetRerunCount()` for the read. The header advertises Tick()
    // as safe from any worker thread, so the mutex is the contract.
    // CodeRabbit #296 finding 2 — `Source_Core/include/PrCheckRunWatcher.h:136-145`.
    mutable std::mutex rerunCountMu_;
    std::map<std::string, int> rerunCountByPrUrl_;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_PR_CHECK_RUN_WATCHER_H
