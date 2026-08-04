#ifndef SMATCHET_DIAGNOSTICS_HOST_MACHINE_INFO_H
#define SMATCHET_DIAGNOSTICS_HOST_MACHINE_INFO_H

// HostMachineInfo — OS / architecture / emulation probes shared by the
// diagnostics surfaces that report "what machine is this running on":
// BugReportService's env payload and the Help > About dialog.
// Extracted verbatim from BugReportService.cpp's anonymous namespace so the two
// surfaces cannot drift apart (a copy would also trip the `duplication` gate).
// docs/plans/active/about-dialog-help-menu.md.

namespace smatchet {
namespace diagnostics {

// Compile-time build target — the OS this binary was built for.
const char* HostOsName();

// Compile-time build target — the architecture this binary was built for. This
// is NOT the native silicon; see DetectHostMachine for that.
const char* HostArchName();

// Runtime host-machine probe. HostArchName above is the compile-time *build*
// target; this reports the *native silicon* and whether the process is being
// emulated on top of it (e.g. an x86_64 build under Prism on an arm64 host).
// Unresolved (Resolved == false) on pre-1709 Windows and non-Windows — callers
// omit the fields rather than guessing.
struct HostMachineInfo {
    const char* NativeArch; // "" when the native machine id is unrecognised
    bool Emulated;
    bool Resolved;
};

HostMachineInfo DetectHostMachine();

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_HOST_MACHINE_INFO_H
