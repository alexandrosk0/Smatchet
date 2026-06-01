// MemoryTelemetry.cpp — platform RSS for the pull-based memory snapshot.
// See MemoryTelemetry.h. Only the Win32 RSS syscall lives here; the rest of the
// MemorySnapshot is assembled by the `perf.memory` command from per-instance
// handles (dispatcher, icon cache, active tickets).

#include "MemoryTelemetry.h"

#if defined(_WIN32)
// `K32GetProcessMemoryInfo` is exported by kernel32 (always linked) — using it
// instead of psapi's `GetProcessMemoryInfo` dodges a psapi.lib dependency that
// the Standalone + DX12 link lines do not otherwise carry. Win7+.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // PROCESS_MEMORY_COUNTERS layout only; the symbol comes from kernel32.
#endif

namespace smatchet {
namespace memtel {

std::atomic<std::size_t>& PendingThumbnailUploads() {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

std::uint64_t ProcessRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (::K32GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    // Non-Windows is not a ship target; return 0 for header honesty rather than
    // pretending to report a value. (Both ship targets are Windows.)
    return 0;
#endif
}

} // namespace memtel
} // namespace smatchet
