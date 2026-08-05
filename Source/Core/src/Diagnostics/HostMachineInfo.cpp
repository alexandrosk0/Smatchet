// HostMachineInfo — see the header. Verbatim extraction from
// BugReportService.cpp's anonymous namespace; behaviour is unchanged.

#include "Diagnostics/HostMachineInfo.h"

#if defined(_WIN32)
// IsWow64Process2 (Win10 1709+) reports the process image machine + the native
// host machine — the only reliable way to tell an x64 build running under arm64
// emulation (Prism) apart from a native build. Resolved dynamically in
// DetectHostMachine so the binary still loads on pre-1709 hosts.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace smatchet {
namespace diagnostics {

const char* HostOsName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

const char* HostArchName() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return sizeof(void*) == 8 ? "64-bit" : "32-bit";
#endif
}

HostMachineInfo DetectHostMachine() {
    HostMachineInfo info{"", false, false};
#if defined(_WIN32)
    typedef BOOL(WINAPI * IsWow64Process2Fn)(HANDLE, USHORT*, USHORT*);
    const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) {
        return info;
    }
    const IsWow64Process2Fn isWow64Process2 =
        reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(k32, "IsWow64Process2"));
    if (isWow64Process2 == nullptr) {
        return info; // pre-Win10 1709 — API absent
    }
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!isWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine)) {
        return info;
    }
    info.Resolved = true;
    // processMachine == UNKNOWN => the process runs natively (not under WOW64 /
    // emulation); any other value is the image machine of an emulated process.
    info.Emulated = processMachine != IMAGE_FILE_MACHINE_UNKNOWN;
    switch (nativeMachine) {
    case IMAGE_FILE_MACHINE_ARM64:
        info.NativeArch = "arm64";
        break;
    case IMAGE_FILE_MACHINE_AMD64:
        info.NativeArch = "x86_64";
        break;
    case IMAGE_FILE_MACHINE_I386:
        info.NativeArch = "x86";
        break;
    default:
        info.NativeArch = "";
        break;
    }
#endif
    return info;
}

} // namespace diagnostics
} // namespace smatchet
