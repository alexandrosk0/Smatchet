#ifndef SMATCHET_STANDALONE_CRASH_HANDLER_H
#define SMATCHET_STANDALONE_CRASH_HANDLER_H

// Standalone crash-handler install. Wires the OS-level crash seams
// (SetUnhandledExceptionFilter / set_terminate / signals) to CrashSink so a
// crash leaves a marker + best-effort minidump for the next-launch reporter.
// docs/plans/shipped/log-a-bug-github.md § Phase 2. Standalone only — the Unreal
// host owns crash handling in the embedded build.

namespace smatchet {

/// Install all crash handlers. Call once at startup AFTER CrashSinkInit.
void InstallCrashHandlers();

/// Install the on-demand self-minidump writer into Source/Core's SelfDump seam, so
/// debug.dump_self can capture every thread's stack from a process that has WEDGED
/// without crashing (docs/adr/0024-self-minidump-over-in-process-stack-walk.md).
/// Standalone-only: this is where dbghelp is already linked, which is precisely why
/// Source/Core carries no Win32 and no dbghelp dependency. Hosts that never call
/// this (Unreal/DX12, Android) simply report the capability as unavailable.
/// Call once at startup, before any thread that can serve commands exists — the
/// seam relies on that ordering for its happens-before edge. No-op off Windows.
void InstallSelfDumpProvider();

#if defined(_WIN32) && defined(_MSC_VER)
// SEH filter for main.cpp's __try/__except around the frame loop. Writes the
// crash marker + minidump, then returns EXCEPTION_EXECUTE_HANDLER so the existing
// handler body still runs. exceptionInfo is the GetExceptionInformation result.
long SmatchetCrashSehFilter(void* exceptionInfo) noexcept;
#endif

} // namespace smatchet

#endif // SMATCHET_STANDALONE_CRASH_HANDLER_H
