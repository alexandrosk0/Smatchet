#ifndef SMATCHET_ICODING_HARNESS_RUNNER_H
#define SMATCHET_ICODING_HARNESS_RUNNER_H

// Pluggable coding-harness runner interface. Phase H3 ships the first concrete
// implementation (`ClaudeCodeLocalRunner`) — additional runners (cloud, Codex,
// Aider) drop in behind this interface without controller changes. Per
// docs/design/agentic-coding-handoff.md § Decisions locked.
//
// Header is always-compiled — declarations only, no AGENTIC-dependent symbols.
// AgenticHandoffController (H4) gates its include via SMATCHET_WITH_AGENTIC.

#include "CodingHarnessTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace CodingHarness {

class IRunner {
  public:
    virtual ~IRunner() = default;

    // Per-event callback for stream-json deltas. Invoked on the worker thread
    // that owns Spawn() — callers reachable from an ImGui frame must marshal
    // back to the UI thread via MainThreadDispatcher (pillar 2).
    using DeltaCallback = std::function<void(const StreamEvent&)>;

    // State-name notification — implementation passes a stable string per
    // HarnessRunState (H4 ships the enum). Same threading rules as DeltaCallback.
    using StateChangeCallback = std::function<void(const std::string& /*newState*/)>;

    // Probe runner availability — checks claude --version, binary on PATH, etc.
    // Returns true if the runner can be used. False + outError documents why.
    // Cheap; safe to call from UI-thread-adjacent code but prefer the worker.
    virtual bool Probe(std::string& outError) = 0;

    // Spawn the harness in the given worktree with the given seed. SYNCHRONOUS;
    // blocks the calling thread until the child exits, the cancel token flips,
    // or the harness writes RUN_RESULT.json + closes stdout. Streams events via
    // onDelta; reports state changes via onStateChange. Returns true on
    // Complete state; false on Failed/Cancelled.
    //
    // Threading: never call from a function reachable from ImGui::*-frame.
    // The controller wraps Spawn() in AppController::LaunchBackgroundTask.
    virtual bool Spawn(const Seed& seed,
                       const std::string& worktreeDir,
                       DeltaCallback onDelta,
                       StateChangeCallback onStateChange,
                       std::shared_ptr<std::atomic<bool>> cancelToken,
                       RunResult& outResult,
                       std::string& outError) = 0;

    // Resume an in-flight harness — H5's clarification reply path and H7's
    // PR-iteration delta both reach the runner through this method. The
    // harness must already be in AwaitingUser or PrOpen state for the runner
    // to honour Resume(); other states return false + outError.
    virtual bool Resume(const ClarificationResponse& response,
                        std::shared_ptr<std::atomic<bool>> cancelToken,
                        std::string& outError) = 0;

    // Stable identifier for logging / config selection (e.g. "claude-code-local").
    // The config's `handoff.runner` string matches against this value.
    virtual std::string Name() const = 0;
};

} // namespace CodingHarness

#endif // SMATCHET_ICODING_HARNESS_RUNNER_H
