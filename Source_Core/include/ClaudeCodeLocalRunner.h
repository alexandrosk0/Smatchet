#ifndef SMATCHET_CLAUDE_CODE_LOCAL_RUNNER_H
#define SMATCHET_CLAUDE_CODE_LOCAL_RUNNER_H

// First concrete `ICodingHarnessRunner` implementation. Spawns `claude` from
// PATH (or an explicit path via Options::binPath) inside an isolated git
// worktree with `bypassPermissions` — the harness's env + cwd are the only
// sandboxing boundary. The runner's job is to materialise that boundary
// safely.
//
// CRITICAL: `bypassPermissions` is a trust boundary, not an OS sandbox.
// The runner's env construction (allow-list) is the actual enforcement;
// the `bypassPermissions` flag merely means the spawned harness doesn't
// prompt the user on each tool call. See AGENTS.md § Handoff envelope and
// docs/design/agentic-flow-implementation.md decision #7.
//
// Header is AGENTIC-gated — the runner consumes ConfigManager fields and
// the CodingHarnessSeedBuilder which both live behind the gate. Call sites
// must `#if defined(SMATCHET_WITH_AGENTIC)` their include.

#include "CodingHarnessTypes.h"
#include "ICodingHarnessRunner.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace CodingHarness {

class ClaudeCodeLocalRunner : public IRunner {
  public:
    struct Options {
        /// Path to the `claude` CLI. Empty = resolve from PATH (the default
        /// for end-users); tests inject the absolute path of `stub_claude.exe`
        /// here. Mirrors `TrackerConfig::HandoffHarnessBinPath` — the runner
        /// receives this verbatim from the controller (H4) which loads it
        /// from config.
        std::string binPath;

        /// Optional pre-spawn `git worktree add` skip — when true, the
        /// runner assumes `worktreeDir` already exists and is on the
        /// requested branch. Useful for tests that prefer a tmp dir over a
        /// real worktree. Production callers leave this false.
        bool skipWorktreeCreate = false;

        /// Wall-clock budget for the entire Spawn() invocation, including
        /// the worker thread on the runner side. 0 = no timeout. The runner
        /// hands this to `SubprocessCapture::CaptureOptions::timeoutMs` for
        /// the child; the runner's own loop adds nothing on top.
        int timeoutMs = 0;

        /// PR-open fallback: when the harness exits with `ok=true` but
        /// `PR_URL.txt` is absent, the runner invokes `git push -u origin
        /// <branch>` followed by `gh pr create --draft --base <prBaseBranch>`
        /// itself, parses the resulting URL from `gh`'s stdout, and writes
        /// it to `PR_URL.txt` so the controller's FSM transition to PrOpen
        /// carries a URL. False disables the fallback — operators who
        /// require harness-written sentinels can flip this off.
        /// Maps to `TrackerConfig::HandoffAutoCreatePrIfMissing`.
        bool autoCreatePrIfMissing = true;

        /// Base branch passed to `gh pr create --base <X>` in the fallback
        /// path. Empty = `develop` (the Smatchet default). Maps to
        /// `TrackerConfig::HandoffPrBaseBranch`.
        std::string prBaseBranch = "develop";

        /// Optional PR body template. Empty = use the built-in template
        /// (carries the `<!-- smatchet-handoff -->` bot-marker). Non-empty
        /// replaces the template; `{proposalId}` / `{issueKey}` /
        /// `{sourceTracker}` placeholders are substituted before passing to
        /// `gh pr create --body`. Maps to `TrackerConfig::HandoffPrBodyTemplate`.
        std::string prBodyTemplate;

        /// Absolute path overrides for `git` and `gh` used by the fallback.
        /// Empty = resolve from PATH (production default). Tests inject the
        /// absolute path of `stub_git.exe` / `stub_gh.exe`. Maps to
        /// `TrackerConfig::HandoffGitBinPath` / `HandoffGhBinPath`.
        std::string gitBinPath;
        std::string ghBinPath;

        /// (coderabbit-react phase-7) Override for the parent directory that
        /// `SpawnAdHoc` creates `coderabbit-pr<N>/` worktrees under. Empty =
        /// the production default `.claude/worktrees` (relative to CWD).
        /// Tests inject an absolute tmp-dir path to keep ad-hoc worktrees
        /// off the actual repo tree.
        std::string adHocWorktreeRoot;
    };

    explicit ClaudeCodeLocalRunner(Options opts);
    ~ClaudeCodeLocalRunner() override;

    // ICodingHarnessRunner.
    bool Probe(std::string& outError) override;
    bool Spawn(const Seed& seed,
               const std::string& worktreeDir,
               DeltaCallback onDelta,
               StateChangeCallback onStateChange,
               std::shared_ptr<std::atomic<bool>> cancelToken,
               RunResult& outResult,
               std::string& outError) override;
    bool Resume(const ClarificationResponse& response,
                std::shared_ptr<std::atomic<bool>> cancelToken,
                std::string& outError) override;
    std::string Name() const override;

    // (coderabbit-react phase-7) Ad-hoc spawn request for non-proposal
    // dispatch sources — CodeRabbit PR-comment dispatches + CI-failure
    // dispatches. Distinct from `Spawn()` (which spawns from an
    // `AgenticHandoffController`-owned `Seed`) in three ways:
    //
    //   1. Worktree path is `.claude/worktrees/coderabbit-pr<N>` (one per PR,
    //      shared between CodeRabbit + CI dispatches), not the
    //      `.claude/worktrees/agent-<proposalId>` shape Spawn() uses.
    //   2. Branch is `coderabbit/pr<N>/iter<n>` (n = iteration count from
    //      `agent_open_pr_watch.iteration_count`), not `agent/<proposalId>/<slug>`.
    //   3. SEED.json carries a `dispatch_source` discriminator (per the
    //      2026-05-18 locked decision); SEED.md opens with a
    //      `## First delegate: handoff-implementer` + `## Routed via:` line.
    //
    // The spawned harness's first move is still `handoff-implementer` — that
    // delegate reads `dispatch_source` to pick a routed sub-delegate
    // (`coderabbit-triage` for `coderabbit_comment`; `build-doctor` /
    // `debug-detective` / `test-rig` for the three `ci_*` sources).
    struct AdHocSpawnRequest {
        /// Canonical PR URL — `https://github.com/<owner>/<repo>/pull/<N>`.
        /// The runner parses `<N>` to derive the worktree dir.
        std::string prUrl;
        /// PR's head ref name (e.g. `agent/42/h7` / `feature/X`). The runner
        /// creates the new iter-branch off `origin/<headRefName>` so the
        /// spawned harness builds on top of the in-flight PR's commits.
        std::string headRefName;
        /// Dispatch-source discriminator — one of:
        ///   `coderabbit_comment` | `ci_build_failure` | `ci_ctest_failure` |
        ///   `ci_coverage_gate`. Written verbatim into SEED.json under the
        ///   new `dispatch_source` key.
        std::string dispatchSource;
        /// For `coderabbit_comment`: the markdown body of the CodeRabbit
        /// comment. For `ci_*`: a short human-readable failure summary
        /// (the full payload lands in `checkRunPayload`).
        std::string commentBodyOrFailureBody;
        /// First-delegate name. Per the 2026-05-18 locked decision this is
        /// **always** `"handoff-implementer"` — kept as a field so the
        /// contract is visible at the call site + future variants stay
        /// future-proof.
        std::string targetAgent;
        /// Routed-via delegate the `handoff-implementer` should hand off
        /// to after reading the seed. One of:
        ///   `coderabbit-triage` | `build-doctor` | `debug-detective` |
        ///   `test-rig`. Written into SEED.md's `## Routed via:` line.
        std::string routedDelegate;
        /// Optional per-spawn iteration counter — sourced from the
        /// `agent_open_pr_watch.iteration_count` column on the PR's row.
        /// Used to suffix the branch name (`coderabbit/pr<N>/iter<n>`).
        /// Defaults to 1 when zero/unset.
        int iteration = 1;
        /// CI-dispatch payload — serialised to `CHECK_RUN.json` in the
        /// worktree root before the spawn. Empty for `coderabbit_comment`
        /// dispatches. Per AGENTS.md § Handoff envelope (phase-7 addition).
        nlohmann::json checkRunPayload;
    };

    /// Run an ad-hoc spawn. Worktree creation, branch shape, SEED.json /
    /// SEED.md / CHECK_RUN.json writes, then forwards to the same spawn
    /// loop `Spawn()` uses. Synchronous — returns when the child exits.
    /// Returns true on `RUN_RESULT.json.ok == true`; otherwise false +
    /// outError carries the rejection cause.
    bool SpawnAdHoc(const AdHocSpawnRequest& req, std::string& outError);

  private:
    Options m_opts;
    // Live worktree directory of the currently-running Spawn(), if any.
    // Resume() writes USER_RESPONSE.json here; protected by m_mu so the
    // controller can call Resume from a different thread than Spawn.
    std::string m_liveWorktreeDir;
    mutable std::mutex m_mu;

    // (coderabbit-react phase-7) Internal helper — invokes the child harness
    // against an already-prepared worktree. SEED.json / SEED.md (+ optional
    // CHECK_RUN.json for `ci_*` ad-hoc dispatches) MUST be written by the
    // caller before calling this. Shared between Spawn() (proposal path,
    // worktree created by this runner + sentinels written from `seed`) and
    // SpawnAdHoc() (ad-hoc path, worktree + sentinels prepared by the
    // ad-hoc-specific code).
    //
    // The auto-PR-create fallback runs unconditionally; for ad-hoc dispatches
    // the harness itself is expected to push to the iter-branch and the
    // operator merges back into the PR's head ref manually.
    bool SpawnCore(const Seed& seed, const std::string& worktreeDir, DeltaCallback onDelta,
                   StateChangeCallback onStateChange, std::shared_ptr<std::atomic<bool>> cancelToken,
                   RunResult& outResult, std::string& outError);
};

} // namespace CodingHarness

#endif // SMATCHET_WITH_AGENTIC

#endif // SMATCHET_CLAUDE_CODE_LOCAL_RUNNER_H
