#include "AiChatTimestamp.h"

#include "TimeNowPure.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace smatchet {
namespace ai {

std::int64_t NowUnixMs() { return TimeNowPure::NowUnixMs(); }

namespace {

constexpr std::int64_t kMsPerMinute = 60LL * 1000LL;
constexpr std::int64_t kMsPerHour = 60LL * kMsPerMinute;
constexpr std::int64_t kMsPerDay = 24LL * kMsPerHour;

const char* MonthAbbrev(int monthIndex0Based) {
    // tm_mon is 0-11. Guard against out-of-range values from a corrupted
    // std::tm so we never index past the array end.
    static const std::array<const char*, 12> kMonths{
        {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}};
    if (monthIndex0Based < 0 || monthIndex0Based > 11) {
        return "???";
    }
    return kMonths[static_cast<std::size_t>(monthIndex0Based)];
}

} // namespace

std::string FormatRelativeTime(std::int64_t nowUnixMs, std::int64_t thenUnixMs) {
    const std::int64_t deltaMs = nowUnixMs - thenUnixMs;
    if (deltaMs < kMsPerMinute) {
        // Future timestamps and sub-minute deltas both land here. Clock skew
        // from a server-issued created-at is the common future-time cause;
        // never surface "-3m ago" to the user.
        return "just now";
    }
    char buf[32];
    if (deltaMs < kMsPerHour) {
        const long long minutes = static_cast<long long>(deltaMs / kMsPerMinute);
        std::snprintf(buf, sizeof(buf), "%lldm ago", minutes);
        return std::string(buf);
    }
    if (deltaMs < kMsPerDay) {
        const long long hours = static_cast<long long>(deltaMs / kMsPerHour);
        std::snprintf(buf, sizeof(buf), "%lldh ago", hours);
        return std::string(buf);
    }
    const long long days = static_cast<long long>(deltaMs / kMsPerDay);
    std::snprintf(buf, sizeof(buf), "%lldd ago", days);
    return std::string(buf);
}

std::string FormatAbsoluteTime(std::int64_t unixMs) {
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm local{};
    // localtime is not thread-safe on its own; use the POSIX/Windows
    // re-entrant variant under the platform-appropriate name.
#if defined(_WIN32)
    if (::localtime_s(&local, &seconds) != 0) {
        return std::string();
    }
#else
    if (::localtime_r(&seconds, &local) == nullptr) {
        return std::string();
    }
#endif
    char buf[32];
    const int year = local.tm_year + 1900;
    const int written = std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d %02d-%s-%04d", local.tm_hour, local.tm_min,
                                      local.tm_sec, local.tm_mday, MonthAbbrev(local.tm_mon), year);
    if (written <= 0) {
        return std::string();
    }
    return std::string(buf);
}

} // namespace ai
} // namespace smatchet
