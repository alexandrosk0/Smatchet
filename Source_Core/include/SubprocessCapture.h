#pragma once

// Cross-platform synchronous subprocess runner. Lifted from
// P4Blame.cpp's RunProcessCapture pair (Win32 + POSIX) into a general
// helper so other call sites (handoff implementer, future agentic
// runners) can reuse the same stdout/stderr capture, byte caps,
// timeout, and cancel-token behaviour.
//
// Threading: Run() is **synchronous** — it blocks the calling thread
// until the child exits, the timeout fires, or the cancel token flips.
// Callers reachable from an ImGui frame MUST wrap this in
// AppController::LaunchBackgroundTask per pillar 2; never call Run()
// directly from the UI thread.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace SubprocessCapture {

struct CaptureResult {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    int64_t durationMs = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool stdoutCapped = false;
    bool stderrCapped = false;
};

struct CaptureOptions {
    /// Full path (or bare exe name to be resolved via PATH / SearchPath)
    /// of the program to spawn.
    std::string argv0;
    std::vector<std::string> args;
    /// Empty = inherit parent's environment.
    std::vector<std::pair<std::string, std::string>> env;
    /// Empty = inherit parent's working directory.
    std::string cwd;
    /// 0 = no timeout. > 0 = wall-clock budget in milliseconds.
    int timeoutMs = 0;
    /// Hard cap on captured stdout bytes. The runner truncates with a
    /// "[capture capped]" suffix once the cap is hit.
    size_t stdoutByteCap = 16u * 1024u * 1024u;
    size_t stderrByteCap = 1u * 1024u * 1024u;
    /// Optional cancel token. When set and the atom flips to true, the
    /// child is terminated and CaptureResult::cancelled is set.
    std::shared_ptr<std::atomic<bool>> cancelToken;
};

/// Run a subprocess, capturing stdout / stderr. Returns true on a
/// successful spawn (regardless of the child's exit code); false on a
/// spawn failure with the OS-level reason in `outError`.
bool Run(const CaptureOptions& opts, CaptureResult& out, std::string& outError);

} // namespace SubprocessCapture
