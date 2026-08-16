#include "Diagnostics/SelfDump.h"

#include <atomic>
#include <string>

namespace smatchet {
namespace diagnostics {

namespace {

// Atomic because the install happens on the startup thread while the read happens on
// whichever thread serves debug.dump_self (an MCP worker, typically). The install is
// sequenced before those threads exist, so the atomic is belt-and-braces rather than
// load-bearing — but it keeps the read race-free even if a host ever installs later.
std::atomic<SelfDumpProvider> g_provider{nullptr};

} // namespace

void SetSelfDumpProvider(SelfDumpProvider fn) noexcept { g_provider.store(fn, std::memory_order_release); }

bool HasSelfDumpProvider() noexcept { return g_provider.load(std::memory_order_acquire) != nullptr; }

bool WriteSelfDump(const std::string& absPath, std::string& errOut) {
    errOut.clear();
    const SelfDumpProvider fn = g_provider.load(std::memory_order_acquire);
    if (fn == nullptr) {
        errOut = "no self-dump provider installed on this host";
        return false;
    }
    if (absPath.empty()) {
        errOut = "empty dump path";
        return false;
    }
    return fn(absPath.c_str(), errOut);
}

std::string MakeSelfDumpFileName(long long epochMs, long long pid) {
    return "ondemand-" + std::to_string(epochMs) + "-" + std::to_string(pid) + ".dmp";
}

} // namespace diagnostics
} // namespace smatchet
