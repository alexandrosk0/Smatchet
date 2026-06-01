#pragma once

// MemoryTelemetry — a thin, pull-based memory-pressure snapshot for the
// `perf.memory` command. Plan: docs/plans/active/memory-budget-and-lifetime-hardening.md § S3.
// Deliberately NOT a push registry: gauges are read at snapshot time only, each
// under its own source's existing lock, so no hot path gains a Report() call.
// This header is backend-agnostic — NO Win32, NO GLFW/GL — so it compiles into
// the DX12 target as well as Standalone. The Win32 RSS syscall lives in the .cpp
// guarded by `#if defined(_WIN32)`, the only TU that links it.
// The struct is assembled by the `perf.memory` command (which holds the
// AppController / dispatcher / cache handles); `ProcessRssBytes()` is the one
// piece this TU owns because it is platform-specific.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace smatchet {
namespace memtel {

/// One memory snapshot. A deliberately-torn cross-source read (each field reads
/// its own source under that source's lock at slightly different instants) — these
/// are gauges for human triage, not invariants, so tearing is acceptable.
struct MemorySnapshot {
    std::uint64_t RssBytes = 0;             ///< Process working set / RSS (0 if unavailable).
    std::size_t DispatcherQueueLen = 0;     ///< MainThreadDispatcher pending tasks right now.
    std::size_t DispatcherLastDrainTasks = 0; ///< Tasks drained in the last frame.
    std::size_t IconCacheEntries = 0;       ///< SmatchetImageTextureCache resident entries.
    std::size_t IconCacheApproxBytes = 0;   ///< Σ Width·Height·4 over resident icon textures (estimate).
    std::size_t ActiveTicketCount = 0;      ///< Published active-ticket count (no copy).
    std::size_t PendingThumbnailUploads = 0; ///< In-flight attachment thumbnail decode→upload tasks (S5).
};

/// Live counter of in-flight thumbnail decode→upload tasks. Incremented by the
/// attachment-preview producer on enqueue, decremented by the dispatcher upload
/// callback (S5). Always linked — the `perf.memory` gauge reads it even when the
/// thumbnail feature is compiled out, in which case it stays 0.
std::atomic<std::size_t>& PendingThumbnailUploads();

/// Resident set size of this process in bytes, or 0 when unavailable (non-Win32
/// stub, or the syscall failed). Win32 uses `K32GetProcessMemoryInfo` (kernel32,
/// always linked) to avoid a psapi.lib dependency.
std::uint64_t ProcessRssBytes();

} // namespace memtel
} // namespace smatchet
