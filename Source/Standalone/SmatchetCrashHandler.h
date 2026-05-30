#ifndef SMATCHET_STANDALONE_CRASH_HANDLER_H
#define SMATCHET_STANDALONE_CRASH_HANDLER_H

// Standalone crash-handler install. Wires the OS-level crash seams
// (SetUnhandledExceptionFilter / set_terminate / signals) to CrashSink so a
// crash leaves a marker + best-effort minidump for the next-launch reporter.
// docs/plans/active/log-a-bug-github.md § Phase 2. Standalone only — the Unreal
// host owns crash handling in the embedded build.

namespace smatchet {

/// Install all crash handlers. Call once at startup AFTER CrashSinkInit.
void InstallCrashHandlers();

#if defined(_WIN32) && defined(_MSC_VER)
// SEH filter for main.cpp's __try/__except around the frame loop. Writes the
// crash marker + minidump, then returns EXCEPTION_EXECUTE_HANDLER so the existing
// handler body (std::exit) still runs. `exceptionInfo` is GetExceptionInformation().
long SmatchetCrashSehFilter(void* exceptionInfo) noexcept;
#endif

} // namespace smatchet

#endif // SMATCHET_STANDALONE_CRASH_HANDLER_H
