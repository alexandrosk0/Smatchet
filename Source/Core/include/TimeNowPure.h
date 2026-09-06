#pragma once

// Wall-clock "now" helpers shared by the TUs that used to carry their own
// identical copies (AI chat timestamps, the backend audit trail, and the
// tracker project-list / field-catalog TTL caches). Unix epoch, system clock —
// callers that need a monotonic clock (e.g. McpPlugin's NowMonotonicMs) are a
// different animal and stay local on purpose.

#include <chrono>
#include <cstdint>

namespace TimeNowPure {

/// Wall-clock "now" in unix-epoch milliseconds.
inline std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Wall-clock "now" in unix-epoch seconds.
inline std::int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace TimeNowPure
