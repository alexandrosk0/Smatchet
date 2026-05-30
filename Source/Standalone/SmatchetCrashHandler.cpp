#include "SmatchetCrashHandler.h"

#include "Diagnostics/CrashSink.h"

#include <csignal>
#include <cstdlib>
#include <exception>

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
void WriteMiniDump(EXCEPTION_POINTERS* exPtrs) noexcept {
    const char* path = diagnostics::CrashSinkPendingDumpPath();
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
    if (exPtrs != nullptr) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exPtrs;
        mei.ClientPointers = FALSE;
        meiPtr = &mei;
    }
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), meiPtr,
                      nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exPtrs) {
    diagnostics::CrashSinkWriteMarkerAsyncSafe("unhandled SEH exception");
    WriteMiniDump(exPtrs);
    // Let default processing continue (terminates the process); next launch reports it.
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void TerminateHandler() {
    diagnostics::CrashSinkWriteMarkerAsyncSafe("std::terminate (unhandled C++ exception)");
#if defined(_WIN32)
    WriteMiniDump(nullptr);
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
    WriteMiniDump(nullptr);
#endif
    std::_Exit(2);
}

} // namespace

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
