#ifndef SMATCHET_DIAGNOSTICS_SELF_DUMP_H
#define SMATCHET_DIAGNOSTICS_SELF_DUMP_H

#include <string>

// SelfDump — on-demand minidump of THIS process, for diagnosing a hang. Backs
// debug.dump_self. Rationale + rejected alternatives: docs/adr/
// 0024-self-minidump-over-in-process-stack-walk.md.
//
// The crash pipeline only fires on a crash, so a process that WEDGES without crashing
// produced no evidence at all. A minidump carries every thread's stack, and
// agents/scripts/core/dump-triage.sh already reads one.
//
// Same split as CrashSink: Source/Core holds the platform-free surface, the OS writer
// lives in Source/Standalone where dbghelp is already linked. Keeping Win32 out of
// here is the point — CORE_SOURCES is globbed into four targets, one of which resolves
// its Win32 libs in Unreal's SmatchetImGuiPlugin.Build.cs, where a missing lib surfaces
// as an editor link failure no CMake CI lane sees.
//
// Threading: unlike HostCallbacks, whose contract is "consumers read on the UI thread",
// this provider is read from whichever thread serves the command — deliberately, so it
// still answers while the UI thread is wedged. Installed once at startup before any
// such thread exists, so the install happens-before every read; the atomic keeps that
// true even if a host ever installs later.

namespace smatchet {
namespace diagnostics {

// Writes a minidump of the current process to `absPath` (absolute, parent directory
// assumed to exist). Returns true on success; on failure fills `errOut` with a
// short, input-free reason. Must not throw.
typedef bool (*SelfDumpProvider)(const char* absPath, std::string& errOut);

/// Install the platform writer. Call once at startup, before starting any thread
/// that can reach WriteSelfDump. Passing nullptr clears it (used by tests).
void SetSelfDumpProvider(SelfDumpProvider fn) noexcept;

/// True when a platform writer is installed. Hosts without one (DX12/Unreal,
/// Android, the POSIX compile gate) report the capability as unavailable rather
/// than failing.
bool HasSelfDumpProvider() noexcept;

/// Write a minidump of this process to `absPath`. Returns false with `errOut` set
/// when no provider is installed or the provider reported failure.
bool WriteSelfDump(const std::string& absPath, std::string& errOut);

/// Deterministic dump filename for an on-demand capture: `ondemand-<epochMs>-<pid>.dmp`.
/// Pure so a test can pin the shape; the command joins it under <userData>agent-dumps/
/// — deliberately not the crash dir, which CrashSink rotates to the 5 newest dumps.
/// Caller supplies the clock + pid so the function stays testable.
std::string MakeSelfDumpFileName(long long epochMs, long long pid);

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_SELF_DUMP_H
