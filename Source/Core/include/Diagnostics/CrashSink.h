#ifndef SMATCHET_DIAGNOSTICS_CRASH_SINK_H
#define SMATCHET_DIAGNOSTICS_CRASH_SINK_H

#include <string>

// CrashSink — the crash-survival substrate for the phase-2 in-process crash
// reporter (docs/plans/active/log-a-bug-github.md § Phase 2). Split so the
// async-signal-safe write path is tiny and dependency-free; the OS crash
// handlers that call it live in Source/Standalone/SmatchetCrashHandler.cpp.
//
// Contract: the *_AsyncSafe path runs inside a crash handler — NO heap, NO
// locks, NO logger, NO std::string. It only touches pre-built static buffers and
// raw OS file calls. Everything else (Init / Breadcrumb / Consume) runs in normal
// context and may allocate.

namespace smatchet {
namespace diagnostics {

struct CrashInfo {
    bool Pending = false;
    std::string Reason;     // short marker reason (e.g. "SEH", "SIGSEGV", "terminate")
    std::string Breadcrumb; // last activity recorded before the crash
    std::string DumpPath;   // archived minidump path, if one was written
    std::string LogTail;    // redacted tail of the CRASHED session's log, if locatable
};

/// Startup (normal context). Builds the static path buffers used by the async
/// handler, ensures `<userDataDir>/crashes` exists, and rotates old dumps. Pass
/// `activeLogPath` = the current session's log-sink path; CrashSink records it so
/// a LATER launch can tail the crashed session's log (it stashes the previous
/// recorded path BEFORE overwriting, only when a crash is pending — avoiding the
/// per-PID-log "can't locate next launch" race). Safe to call once before
/// installing handlers. Idempotent.
void CrashSinkInit(const std::string& userDataDir, const std::string& activeLogPath = std::string());

/// Normal context. Record what the user is doing now (cheap file overwrite) so a
/// later crash report can say what was happening. No-op before Init.
void CrashSinkBreadcrumb(const char* activity);

/// Async-signal-safe. Write the crash marker file (so the next launch knows a
/// crash happened). `reason` MUST be a short string literal. Never allocates.
void CrashSinkWriteMarkerAsyncSafe(const char* reason) noexcept;

/// Pre-built absolute path the crash handler should write the minidump to
/// (stable buffer; valid after Init). Returns "" before Init.
const char* CrashSinkPendingDumpPath() noexcept;

/// Next launch (normal context). True when a crash marker is present.
bool CrashSinkHasPending();

/// Next launch (normal context). Read marker + breadcrumb + log tail, archive the
/// pending dump (timestamped), DELETE the marker (so we never loop), and return
/// the assembled info. Returns {Pending=false} when nothing is pending.
CrashInfo CrashSinkConsume();

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_CRASH_SINK_H
