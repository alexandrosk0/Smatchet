#include "CompactDateFormat.h"
#include "UiPerfMonitor.h"
#include "imgui.h"

// For timegm() on glibc / BSD (not MSVC).
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <unordered_map>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace {

std::string TrimCopy(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

// NOTE: TryParseJiraDateTime / FormatJiraDateOrDateTimeForApi and their dedicated
// helpers (IsDigit / ParseTimeZone / SkipFractional / AppendOffsetJira) now live in
// CompactDateFormatPure.cpp so they link into the ImGui-free TSan subset. The display
// path below keeps its own TrimCopy and calls TryParseJiraDateTime across the TU.

std::time_t TimeGmPortable(std::tm* tmUtc) {
#if defined(_WIN32)
    return _mkgmtime(tmUtc);
#else
    return timegm(tmUtc);
#endif
}

bool ParsedToTm(const ParsedJiraDateTime& p, std::tm& outTm) {
    std::memset(&outTm, 0, sizeof(outTm));
    if (p.Year < 1 || p.Month < 1 || p.Month > 12 || p.Day < 1 || p.Day > 31) {
        return false;
    }
    outTm.tm_year = p.Year - 1900;
    outTm.tm_mon = p.Month - 1;
    outTm.tm_mday = p.Day;
    outTm.tm_hour = p.HasWallTime ? p.Hour : 0;
    outTm.tm_min = p.HasWallTime ? p.Minute : 0;
    outTm.tm_sec = p.HasWallTime ? p.Second : 0;
    outTm.tm_isdst = 0;
    return true;
}

std::string FormatShortLocalDate(const std::chrono::system_clock::time_point& tp) {
    using namespace std::chrono;
    const std::time_t tt = system_clock::to_time_t(tp);
    std::tm tmLocal{};
#if defined(_WIN32)
    if (localtime_s(&tmLocal, &tt) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&tt, &tmLocal) == nullptr) {
        return std::string();
    }
#endif
    char buf[64];
    const size_t n = std::strftime(buf, sizeof(buf), "%b %d '%y", &tmLocal);
    if (n == 0) {
        return std::string();
    }
    // Strip the leading zero from the day field so output is e.g. "Jan 5 '26",
    // not "Jan 05 '26". Avoids the MSVC-only %#d / GNU-only %-d strftime flags
    // — GCC -Wformat trips on %#d under MinGW UCRT64.
    std::string s(buf, n);
    const size_t spc = s.find(' ');
    if (spc != std::string::npos && spc + 1 < s.size() && s[spc + 1] == '0') {
        s.erase(spc + 1, 1);
    }
    return s;
}

std::string FormatAbsoluteIso(const std::chrono::system_clock::time_point& tp, bool hasTime) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tmLocal{};
#if defined(_WIN32)
    if (localtime_s(&tmLocal, &tt) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&tt, &tmLocal) == nullptr) {
        return std::string();
    }
#endif
    char buf[64];
    size_t n = 0;
    if (hasTime) {
        n = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    } else {
        n = std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmLocal);
    }
    return (n == 0) ? std::string() : std::string(buf, n);
}

std::string FormatAbsoluteFriendly(const std::chrono::system_clock::time_point& tp, bool hasTime) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tmLocal{};
#if defined(_WIN32)
    if (localtime_s(&tmLocal, &tt) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&tt, &tmLocal) == nullptr) {
        return std::string();
    }
#endif
    char buf[128];
    size_t n = 0;
    if (hasTime) {
        n = std::strftime(buf, sizeof(buf), "%b %d, %Y, %H:%M", &tmLocal);
    } else {
        n = std::strftime(buf, sizeof(buf), "%b %d, %Y", &tmLocal);
    }
    return (n == 0) ? std::string() : std::string(buf, n);
}

struct ParsedDisplayTimePoint {
    std::chrono::system_clock::time_point Tp;
    bool HasWallTime = false;
    bool Success = false;
};

// Parse `raw` to a UTC time_point (cached, thread-local). Mirrors the inline
// CachedParsedDate logic lifted out of FormatCompactJiraDateForDisplay verbatim.
ParsedDisplayTimePoint ParseDisplayTimePointCached(const std::string& raw) {
    thread_local static std::unordered_map<std::string, ParsedDisplayTimePoint> s_parseCache;

    auto it = s_parseCache.find(raw);
    if (it != s_parseCache.end()) {
        return it->second;
    }

    ParsedDisplayTimePoint cp;
    const std::string s = TrimCopy(raw);
    if (!s.empty()) {
        ParsedJiraDateTime p;
        if (TryParseJiraDateTime(s, p)) {
            std::tm wall{};
            if (ParsedToTm(p, wall)) {
                const int offsetSec = p.HasTimeZoneSuffix ? p.OffsetSec : 0;
                const std::time_t wallAsUtcGuess = TimeGmPortable(&wall);
                if (wallAsUtcGuess != static_cast<std::time_t>(-1)) {
                    cp.Tp =
                        std::chrono::system_clock::from_time_t(wallAsUtcGuess - static_cast<std::time_t>(offsetSec));
                    cp.HasWallTime = p.HasWallTime;
                    cp.Success = true;
                }
            }
        }
    }
    if (s_parseCache.size() > 8192) {
        s_parseCache.clear();
    }
    s_parseCache[raw] = cp;
    return cp;
}

// Relative-magnitude label ("-3d", "+2w", "0m") for a non-absolute format option.
// `formatOption == "compact"` switches to a short local date once `mag` reaches the
// threshold; on short-date failure it falls back to `raw`.
std::string FormatRelativeMagnitude(const std::chrono::system_clock::time_point& tp, const std::string& raw,
                                    const std::string& formatOption, int thresholdDays) {
    const auto now = std::chrono::system_clock::now();
    const auto diffSec = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();

    constexpr long long kMinute = 60;
    constexpr long long kHour = 3600;
    constexpr long long kDay = 86400;
    constexpr long long kWeek = 604800;
    constexpr long long kMonth = 2592000;
    constexpr long long kYear = 31536000;

    const long long mag = (diffSec < 0) ? static_cast<long long>(-diffSec) : static_cast<long long>(diffSec);
    const std::string sign = (diffSec >= 0) ? "-" : "+";

    if (formatOption == "compact") {
        const long long thresholdSec = static_cast<long long>(thresholdDays) * kDay;
        if (mag >= thresholdSec) {
            const std::string shortDate = FormatShortLocalDate(tp);
            return shortDate.empty() ? raw : shortDate;
        }
    }

    if (mag >= kYear) {
        return sign + std::to_string(mag / kYear) + "y";
    }
    if (mag >= kMonth) {
        return sign + std::to_string(mag / kMonth) + "mo";
    }
    if (mag >= kWeek) {
        return sign + std::to_string(mag / kWeek) + "w";
    }
    if (mag >= kDay) {
        return sign + std::to_string(mag / kDay) + "d";
    }
    if (mag >= kHour) {
        return sign + std::to_string(mag / kHour) + "h";
    }
    if (mag >= kMinute) {
        return sign + std::to_string(mag / kMinute) + "m";
    }
    if (mag > 0) {
        return sign + "1m";
    }
    return "0m";
}

// Full value computation (parse + format-option dispatch) for one date string.
std::string ComputeCompactJiraDisplay(const std::string& raw, const std::string& formatOption, int thresholdDays) {
    const ParsedDisplayTimePoint cp = ParseDisplayTimePointCached(raw);
    if (!cp.Success) {
        return raw;
    }

    const auto& tp = cp.Tp;
    if (formatOption == "absolute_iso") {
        const std::string val = FormatAbsoluteIso(tp, cp.HasWallTime);
        return val.empty() ? raw : val;
    }
    if (formatOption == "absolute_friendly") {
        const std::string val = FormatAbsoluteFriendly(tp, cp.HasWallTime);
        return val.empty() ? raw : val;
    }
    return FormatRelativeMagnitude(tp, raw, formatOption, thresholdDays);
}

} // namespace

// TryParseJiraDateTime + FormatJiraDateOrDateTimeForApi: see CompactDateFormatPure.cpp.

std::string FormatCompactJiraDateForDisplay(const std::string& raw, const std::string& formatOption,
                                            int thresholdDays) {
    SMATCHET_UI_PERF_SCOPE("FormatCompactJiraDateForDisplay");

    struct CachedFormattedDate {
        std::string formatted;
        double lastUpdatedSec = 0.0;
        int lastFrameCount = -1;
        std::string formatOption;
        int thresholdDays = 0;
    };
    thread_local static std::unordered_map<std::string, CachedFormattedDate> s_formatCache;

    const ImGuiContext* ctx = ImGui::GetCurrentContext();
    const int frameCount = ctx ? ImGui::GetFrameCount() : -1;

    auto formatIt = s_formatCache.find(raw);
    if (formatIt != s_formatCache.end()) {
        CachedFormattedDate& entry = formatIt->second;
        // Within the same frame the date string cannot change: skip everything.
        if (frameCount >= 0 && entry.lastFrameCount == frameCount && entry.formatOption == formatOption &&
            entry.thresholdDays == thresholdDays) {
            return entry.formatted;
        }
        // Cross-frame: check 2-second TTL so relative labels ("-3d") stay fresh.
        const double nowSec = ctx ? ImGui::GetTime()
                                  : static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                            .count());
        if (entry.formatOption == formatOption && entry.thresholdDays == thresholdDays &&
            (nowSec - entry.lastUpdatedSec) < 2.0) {
            entry.lastFrameCount = frameCount;
            return entry.formatted;
        }
    }

    const double nowSec = ctx ? ImGui::GetTime()
                              : static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::steady_clock::now().time_since_epoch())
                                                        .count());
    std::string formattedVal = ComputeCompactJiraDisplay(raw, formatOption, thresholdDays);
    if (s_formatCache.size() > 8192) {
        s_formatCache.clear();
    }
    s_formatCache[raw] = CachedFormattedDate{formattedVal, nowSec, frameCount, formatOption, thresholdDays};
    return formattedVal;
}
