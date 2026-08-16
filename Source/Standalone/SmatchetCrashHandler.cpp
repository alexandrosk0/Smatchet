#include "SmatchetCrashHandler.h"

#include "Diagnostics/CrashSink.h"
#include "Diagnostics/SelfDump.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <string>

#if defined(_WIN32)
#include <windows.h>
// dbghelp.h must follow windows.h.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace smatchet {

namespace {

#if defined(_WIN32)
// Best-effort minidump. Heavy + not strictly async-signal-safe, but the marker
// is already written by the time we get here, so a failed/hung dump never costs
// us the crash detection. `exPtrs` may be null (signal path) — MiniDumpWriteDump
// still captures the faulting thread's state.
// True once a dump carrying an exception stream (exPtrs != null) has been written. A later
// exception-less dump (terminate / signal path, exPtrs == null) must NOT clobber it: the
// frame-loop SEH filter writes the richest dump first (faulting instruction + context), and
// the std::exit() it triggers used to run a terminate that overwrote it with a stream-less
// one — destroying the only diagnostic the next-launch reporter has. First-rich-dump wins.
std::atomic<bool> g_exceptionDumpWritten{false};

// Synthetic exception code for dumps written off the terminate/signal path, where the OS gives
// us no EXCEPTION_RECORD. The high bit (0x8000'0000 = severity ERROR) + the customer bit
// (0x2000'0000) form a valid non-system NTSTATUS-style code so a debugger/minidump reader treats
// the ExceptionStream as a real (if app-synthesized) record. 'S' = Smatchet.
const DWORD kSyntheticTerminateException = 0xE0535343; // 0xE0000000 | 'SCS'

// Whether a dump's exception record was OS-supplied (real SEH) vs app-synthesized (terminate/signal).
enum class DumpExceptionOrigin { Real, Synthetic };

#if defined(_WIN32)
// The dump TYPE is the security-relevant decision, and it is the one thing the crash
// path and the on-demand self-dump path below MUST agree on — everything else about
// them legitimately differs. Named once so neither writer can drift: the richer scopes
// (MiniDumpWithIndirectlyReferencedMemory / MiniDumpScanMemory) walk the stack for
// pointers and pull the referenced heap in, which sweeps in-memory secrets (config API
// tokens, GitHub PAT, MCP auth token, AI keys held in std::string) into a .dmp that the
// bug reporter auto-attaches to an off-host report. See the audit note at the
// MiniDumpWriteDump call below.
const MINIDUMP_TYPE kSmatchetDumpType = MiniDumpNormal;
#endif

// `realExPtrs` is the OS-supplied EXCEPTION_POINTERS (SEH path) or null. `origin` records whether
// the record is OS-supplied or app-synthesized so the first-rich-dump-wins arbitration treats a
// synthetic terminate/signal dump with the same (lower) priority the old exPtrs==null path had —
// a later real SEH dump must still be able to win, and a synthetic dump must not clobber a prior
// real one.
void WriteMiniDumpImpl(EXCEPTION_POINTERS* realExPtrs, DumpExceptionOrigin origin) noexcept {
    const char* path = diagnostics::CrashSinkPendingDumpPath();
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    const bool isRealException = (origin == DumpExceptionOrigin::Real) && (realExPtrs != nullptr);
    if (!isRealException && g_exceptionDumpWritten.load(std::memory_order_acquire)) {
        return; // preserve the earlier (real) exception-bearing dump
    }
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    // On the terminate/signal path the OS hands us no EXCEPTION_RECORD, so a MiniDumpNormal with a
    // null MINIDUMP_EXCEPTION_INFORMATION carries NO ExceptionStream — the dump is barely
    // analyzable (no faulting thread/context marked). Synthesize one: RtlCaptureContext snapshots
    // the current thread's registers at the crash site, and a minimal EXCEPTION_RECORD with our
    // synthetic code gives the reader a real ExceptionStream to anchor on.
    CONTEXT synthCtx;
    EXCEPTION_RECORD synthRec;
    EXCEPTION_POINTERS synthPtrs;
    EXCEPTION_POINTERS* exPtrs = realExPtrs;
    if (exPtrs == nullptr) {
        ZeroMemory(&synthCtx, sizeof(synthCtx));
        synthCtx.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&synthCtx);
        ZeroMemory(&synthRec, sizeof(synthRec));
        synthRec.ExceptionCode = kSyntheticTerminateException;
        // EXCEPTION_NONCONTINUABLE: the process is going down right after this dump.
        synthRec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64) || defined(__x86_64__)
        synthRec.ExceptionAddress = reinterpret_cast<PVOID>(synthCtx.Rip);
#elif defined(_M_IX86) || defined(__i386__)
        synthRec.ExceptionAddress = reinterpret_cast<PVOID>(synthCtx.Eip);
#elif defined(_M_ARM64) || defined(__aarch64__)
        synthRec.ExceptionAddress = reinterpret_cast<PVOID>(synthCtx.Pc);
#else
        synthRec.ExceptionAddress = nullptr;
#endif
        synthPtrs.ExceptionRecord = &synthRec;
        synthPtrs.ContextRecord = &synthCtx;
        exPtrs = &synthPtrs;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
    if (exPtrs != nullptr) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exPtrs;
        mei.ClientPointers = FALSE;
        meiPtr = &mei;
    }
    // Audit 2026-06-13 synthesis #17: MiniDumpNormal (stack + thread context + module list only),
    // NOT MiniDumpWithIndirectlyReferencedMemory|MiniDumpScanMemory. The richer scopes walk the
    // stack for pointers and pull the referenced heap into the dump — which sweeps in-memory
    // secrets (config API tokens, GitHub PAT, MCP auth token, AI keys held in std::string on the
    // heap) into a .dmp the bug-reporter auto-attaches to an off-host crash report. Normal still
    // carries the faulting thread's stack + the exception record (meiPtr), which is what the
    // next-launch triage actually needs; the lost heap context is not worth the secret-spill risk.
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kSmatchetDumpType, meiPtr, nullptr, nullptr);
    CloseHandle(file);
    // Only a REAL (OS-supplied) exception record marks "rich dump written" — a synthesized
    // terminate/signal record stays low-priority so a subsequent real SEH dump can still win.
    if (isRealException) {
        g_exceptionDumpWritten.store(true, std::memory_order_release);
    }
}

// SEH path: OS-supplied EXCEPTION_POINTERS — a real exception record.
void WriteMiniDump(EXCEPTION_POINTERS* exPtrs) noexcept { WriteMiniDumpImpl(exPtrs, DumpExceptionOrigin::Real); }

// terminate/signal path: no OS record — synthesize one (captured context + synthetic code) so the
// dump still carries an analyzable ExceptionStream instead of being context-less.
void WriteSyntheticMiniDump() noexcept { WriteMiniDumpImpl(nullptr, DumpExceptionOrigin::Synthetic); }

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exPtrs) {
    diagnostics::CrashSinkWriteMarkerAsyncSafe("unhandled SEH exception");
    WriteMiniDump(exPtrs);
    // Let default processing continue (terminates the process); next launch reports it.
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void TerminateHandler() {
    // #987 review MEDIUM-1: write the crash MARKER first — it is the keystone
    // (crash *detection* on next boot), and a hang in the breadcrumb append
    // below (fopen/fwrite AV-lock, disk-full stall) must not cost us detection.
    // The marker write is async-safe; the heavier breadcrumb capture follows.
    diagnostics::CrashSinkWriteMarkerAsyncSafe("std::terminate (unhandled C++ exception)");
    // The terminate path writes dumps with NO exception stream — capture the
    // active exception's what() into the breadcrumb so the dump isn't just
    // "std::terminate" with no clue WHICH exception escaped. Rethrow is the only
    // portable way to inspect a current_exception in C++14. Kept minimal: one
    // string copy into the breadcrumb file, no logger (unsafe mid-crash).
    try {
        std::exception_ptr active = std::current_exception();
        if (active) {
            std::rethrow_exception(active);
        }
    } catch (const std::exception& e) {
        diagnostics::CrashSinkAppendBreadcrumbLine("terminate: ", e.what());
    } catch (...) {
        // Non-std exception type — record that fact (an empty catch-all here
        // would silently drop the only diagnostic this path exists to capture).
        diagnostics::CrashSinkAppendBreadcrumbLine("terminate: ", "non-std exception type (no what())");
    }
#if defined(_WIN32)
    WriteSyntheticMiniDump();
#endif
    std::_Exit(3);
}

void SignalHandler(int sig) {
    const char* reason = "signal";
    switch (sig) {
    case SIGSEGV:
        reason = "SIGSEGV (segfault)";
        break;
    case SIGABRT:
        reason = "SIGABRT (abort)";
        break;
    case SIGFPE:
        reason = "SIGFPE (math error)";
        break;
    case SIGILL:
        reason = "SIGILL (illegal instruction)";
        break;
    default:
        break;
    }
    diagnostics::CrashSinkWriteMarkerAsyncSafe(reason);
#if defined(_WIN32)
    WriteSyntheticMiniDump();
#endif
    std::_Exit(2);
}

} // namespace

#if defined(_WIN32)
namespace {

// On-demand self-dump provider behind Source/Core's SelfDump seam. This is NOT the
// crash path and deliberately shares none of its machinery. Nothing has faulted, so
// there is no exception record to attach — passing none is correct here, and every
// thread's stack is still captured, which is the whole point. There is no
// first-rich-dump arbitration either, because that arbitrates between CRASH dumps
// and must not let an on-demand capture suppress a later real one. And the caller
// owns the path rather than CrashSink's pre-built buffer. The one thing the two
// writers share is kSmatchetDumpType.
//
// Runs on whichever thread served the command, by design — the target case is a
// wedged UI thread. MiniDumpWriteDump suspends the other threads itself for the
// duration, so this never hand-rolls thread suspension.
bool WriteSelfDumpWin32(const char* absPath, std::string& errOut) noexcept {
    if (absPath == nullptr || absPath[0] == '\0') {
        errOut = "empty dump path";
        return false;
    }
    HANDLE file = CreateFileA(absPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // Input-free message: the caller already knows the path it asked for, and the
        // OS code is the part it cannot see.
        errOut = "could not create dump file (win32 error " +
                 std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
        return false;
    }
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kSmatchetDumpType, nullptr,
                                      nullptr, nullptr);
    if (ok == FALSE) {
        errOut =
            "MiniDumpWriteDump failed (win32 error " + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    }
    CloseHandle(file);
    return ok != FALSE;
}

} // namespace
#endif // _WIN32

void InstallSelfDumpProvider() {
#if defined(_WIN32)
    diagnostics::SetSelfDumpProvider(&WriteSelfDumpWin32);
#endif
    // Off Windows there is no writer; Source/Core reports the capability as absent.
}

void InstallCrashHandlers() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(TopLevelExceptionFilter);
#endif
    std::set_terminate(TerminateHandler);
    std::signal(SIGSEGV, SignalHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGFPE, SignalHandler);
    std::signal(SIGILL, SignalHandler);
}

#if defined(_WIN32) && defined(_MSC_VER)
long SmatchetCrashSehFilter(void* exceptionInfo) noexcept {
    diagnostics::CrashSinkWriteMarkerAsyncSafe("SEH exception in frame loop");
    WriteMiniDump(static_cast<EXCEPTION_POINTERS*>(exceptionInfo));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace smatchet
