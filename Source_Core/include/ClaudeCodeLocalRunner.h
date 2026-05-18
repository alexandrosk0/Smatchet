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

  private:
    Options m_opts;
    // Live worktree directory of the currently-running Spawn(), if any.
    // Resume() writes USER_RESPONSE.json here; protected by m_mu so the
    // controller can call Resume from a different thread than Spawn.
    std::string m_liveWorktreeDir;
    mutable std::mutex m_mu;
};

} // namespace CodingHarness

#endif // SMATCHET_WITH_AGENTIC

#endif // SMATCHET_CLAUDE_CODE_LOCAL_RUNNER_H
