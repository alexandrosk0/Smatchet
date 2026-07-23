#pragma once

// Cross-platform synchronous subprocess runner. Lifted from
// P4Annotate.cpp's RunProcessCapture pair (Win32 + POSIX) into a general
// helper so other call sites can reuse the same stdout/stderr capture,
// byte caps, timeout, and cancel-token behaviour.
// Threading: Run() is **synchronous** — it blocks the calling thread
// until the child exits, the timeout fires, or the cancel token flips.
// Callers reachable from an ImGui frame MUST wrap this in
// AppController::LaunchBackgroundTask per pillar 2; never call Run()
// directly from the UI thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SmatchetResult.h" // Result<CaptureResult> (Run)

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
    /// When true AND `env` is non-empty, the child sees ONLY the entries in
    /// `env` — every other variable inherited from the parent is dropped.
    /// On Windows this is the natural CreateProcessW behaviour (envPtr is the
    /// child's full block). On POSIX the runner calls `clearenv()` before
    /// `setenv()` so the same allow-list semantic holds. False (the default)
    /// keeps the additive-merge behaviour the P4Annotate call sites expect.
    bool replaceParentEnv = false;
    /// When true, the child does NOT inherit parent env vars whose name looks
    /// secret-bearing (TOKEN / SECRET / PASSWORD / KEY / _PAT / …, see
    /// SubprocessCapturePure::IsSensitiveEnvName). PATH / SYSTEMROOT / TEMP /
    /// locale / HOME / P4* / GIT* all survive, so p4, git and the file-pickers
    /// keep working. A proportionate defense-in-depth scrub for audit synthesis
    /// #15 (same-user is inside the trust boundary, so the goal is to avoid
    /// gratuitously handing a child every API token, not to build a sandbox).
    /// Ignored when `replaceParentEnv` is set (that path already gives the child
    /// only the explicit `env` entries). `env` overrides still apply on top.
    bool scrubSensitiveEnv = false;
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

    /// Optional line-streaming callback. When set, invoked once per
    /// newline-terminated line read from the child's stdout — the line
    /// argument has the trailing '\n' stripped. Lines that exceed the
    /// `stdoutByteCap` (after which output is truncated) are NOT delivered
    /// past the cap. The callback fires on the same thread that called
    /// Run(); never marshal directly to ImGui (pillar 2). Lines are also
    /// appended to `CaptureResult::stdoutText` as usual — the callback is
    /// additive, not a redirect. An unset callback is the P4Annotate
    /// buffered-capture path.
    std::function<void(const std::string&)> onStdoutLine;
};

/// Run a subprocess, capturing stdout / stderr. `Ok(CaptureResult)` on a
/// successful spawn (regardless of the child's exit code — a non-zero child
/// exit is still success, with the code in `CaptureResult::exitCode`);
/// `Err(reason)` on a spawn failure with the OS-level reason.
Result<CaptureResult> Run(const CaptureOptions& opts);

} // namespace SubprocessCapture
