// MemoryTelemetry.test.cpp — S3 of
// docs/plans/active/memory-budget-and-lifetime-hardening.md.
//
// Bucket-A coverage for the pull-based memory snapshot primitives in
// Source/Core/include/MemoryTelemetry.h + Source/Core/src/Persistence/MemoryTelemetry.cpp:
//
//   - ProcessRssBytes() returns a plausible non-zero RSS on Windows (the ship
//     target; via K32GetProcessMemoryInfo) and the documented 0 stub elsewhere.
//   - PendingThumbnailUploads() is a process-wide atomic that starts at 0 and
//     round-trips through fetch_add / fetch_sub (the S5 gauge the producer
//     increments on enqueue and the dispatcher upload callback decrements).
//
// Pure C++ / no UI thread / no HTTP / no SQLite — runs in the doctest rig.

#include "MemoryTelemetry.h"

#include "doctest/doctest.h"

TEST_CASE("ProcessRssBytes reports a plausible resident set") {
#if defined(_WIN32)
    // Win32 ship target: a live process always has a non-zero working set.
    CHECK(smatchet::memtel::ProcessRssBytes() > 0u);
#else
    // Documented stub on non-ship platforms — returns 0 for header honesty.
    CHECK(smatchet::memtel::ProcessRssBytes() == 0u);
#endif
}

TEST_CASE("PendingThumbnailUploads counter round-trips") {
    std::atomic<std::size_t>& counter = smatchet::memtel::PendingThumbnailUploads();
    const std::size_t base = counter.load();
    counter.fetch_add(1, std::memory_order_relaxed);
    CHECK(counter.load() == base + 1u);
    counter.fetch_sub(1, std::memory_order_relaxed);
    CHECK(counter.load() == base);
}

TEST_CASE("PendingThumbnailUploads returns the same singleton each call") {
    // The producer + the upload callback + the perf.memory gauge must all see
    // ONE counter — a fresh atomic per call would make the gauge meaningless.
    CHECK(&smatchet::memtel::PendingThumbnailUploads() == &smatchet::memtel::PendingThumbnailUploads());
}
