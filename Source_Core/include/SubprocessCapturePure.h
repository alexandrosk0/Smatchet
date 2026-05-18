#pragma once

// Pure C++14 helpers used by the platform-specific SubprocessCapture
// implementation: argv quoting, env-block construction, and
// monotonic-clock timeout arithmetic. No platform headers, no I/O —
// every helper is deterministic so the doctest rig can exercise the
// gnarly quoting / env-ordering corner cases without spawning a process.

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SubprocessCapturePure {

/// Quote a single argv element so it round-trips through Windows'
/// CommandLineToArgvW parser (the rule set used by CreateProcessW when
/// the caller passes a single command-line buffer rather than an argv
/// vector). Backslashes preceding a literal quote must be doubled per
/// the MSC quoting rules — see MSDN "Parsing C++ command-line arguments"
/// and ParseCmdLineW.
///
/// Returns the input unchanged when it contains no whitespace and no
/// quote characters (the caller can paste it into a command line
/// verbatim). Otherwise wraps it in double quotes and escapes embedded
/// quotes and the backslash runs that precede them.
std::string QuoteArgvWindows(const std::string& arg);

/// Quote a single argv element per POSIX shell rules using single-quote
/// wrapping. Single quotes are escaped via the `'\''` trick. Used for
/// diagnostic logging on POSIX — the real exec path uses execvp() with
/// a char*[] argv, so quoting there is purely cosmetic for log lines
/// the user pastes back into a shell.
std::string QuoteArgvPosix(const std::string& arg);

/// Build a Windows CreateProcessW-style env block: a string of
/// `KEY=VALUE\0KEY=VALUE\0\0` segments, double-null terminated.
/// Caller passes the .data() pointer as the lpEnvironment argument.
/// Order is preserved (Windows itself does not care, but tests assert
/// it for determinism).
std::string BuildEnvBlockWindows(const std::vector<std::pair<std::string, std::string>>& env);

/// Compute milliseconds remaining given a start time and a total
/// timeout budget. Returns 0 when expired, and the caller-supplied
/// totalTimeoutMs when totalTimeoutMs <= 0 (i.e. "no timeout").
int64_t RemainingTimeoutMs(std::chrono::steady_clock::time_point start, int64_t totalTimeoutMs);

} // namespace SubprocessCapturePure
