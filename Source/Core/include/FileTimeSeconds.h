#pragma once

// Whole seconds since a clock's epoch, for comparing a file's modification time with "now" read from the
// same clock (the file-time clock of ghc filesystem). The epoch differs between platforms and standard
// libraries, so a file time must never be compared with system_clock. Header-only, no dependencies.

#include <chrono>
#include <cstdint>

namespace smatchet {
namespace filetime {

template <class TimePoint> std::int64_t ToEpochSeconds(const TimePoint& t) {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

/// `Clock::now()` in whole seconds; pass the file-time clock as the template argument.
template <class Clock> std::int64_t NowEpochSeconds() { return ToEpochSeconds(Clock::now()); }

} // namespace filetime
} // namespace smatchet
