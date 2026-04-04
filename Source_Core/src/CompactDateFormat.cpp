#include "CompactDateFormat.h"

// For timegm() on glibc / BSD (not MSVC).
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>

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

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

// Parse optional timezone: Z | ±HHMM | ±HH:MM. Returns true and sets offset seconds east of UTC (+01:00 -> +3600).
bool ParseTimeZone(const std::string& s, size_t& i, int& offsetSec) {
    offsetSec = 0;
    if (i >= s.size()) {
        return true; // no tz -> treat as UTC (Jira often omits on date-only)
    }
    if (s[i] == 'Z' || s[i] == 'z') {
        ++i;
        return true;
    }
    if (s[i] != '+' && s[i] != '-') {
        return false;
    }
    const int sign = (s[i] == '-') ? -1 : 1;
    ++i;
    int oh = 0;
    int om = 0;
    if (i + 2 > s.size() || !IsDigit(s[i]) || !IsDigit(s[i + 1])) {
        return false;
    }
    oh = (s[i] - '0') * 10 + (s[i + 1] - '0');
    i += 2;
    if (i < s.size() && s[i] == ':') {
        ++i;
        if (i + 2 > s.size() || !IsDigit(s[i]) || !IsDigit(s[i + 1])) {
            return false;
        }
        om = (s[i] - '0') * 10 + (s[i + 1] - '0');
        i += 2;
    } else if (i + 2 <= s.size() && IsDigit(s[i]) && IsDigit(s[i + 1])) {
        om = (s[i] - '0') * 10 + (s[i + 1] - '0');
        i += 2;
    } else {
        om = 0;
    }
    offsetSec = sign * (oh * 3600 + om * 60);
    return true;
}

// Parse fractional seconds (if present) and skip; does not affect our second resolution.
void SkipFractional(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '.') {
        return;
    }
    ++i;
    while (i < s.size() && IsDigit(s[i])) {
        ++i;
    }
}

// Wall time in the zone indicated by offsetSec (east of UTC). Returns false on parse error.
bool ParseJiraDateTime(const std::string& s, std::tm& outTm, int& offsetSec) {
    std::memset(&outTm, 0, sizeof(outTm));
    offsetSec = 0;
    if (s.size() < 10) {
        return false;
    }
    if (!IsDigit(s[0]) || !IsDigit(s[1]) || !IsDigit(s[2]) || !IsDigit(s[3]) || s[4] != '-' ||
        !IsDigit(s[5]) || !IsDigit(s[6]) || s[7] != '-' || !IsDigit(s[8]) || !IsDigit(s[9])) {
        return false;
    }
    int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int mon = (s[5] - '0') * 10 + (s[6] - '0');
    int day = (s[8] - '0') * 10 + (s[9] - '0');
    size_t i = 10;
    int hour = 0;
    int min = 0;
    int sec = 0;

    if (i >= s.size() || s[i] != 'T') {
        // Date-only (e.g. duedate): midnight in specified / default UTC.
        outTm.tm_year = year - 1900;
        outTm.tm_mon = mon - 1;
        outTm.tm_mday = day;
        outTm.tm_hour = 0;
        outTm.tm_min = 0;
        outTm.tm_sec = 0;
        if (i < s.size()) {
            if (!ParseTimeZone(s, i, offsetSec)) {
                return false;
            }
        }
        return i == s.size();
    }

    ++i; // skip T
    if (i + 8 > s.size() || !IsDigit(s[i]) || !IsDigit(s[i + 1]) || s[i + 2] != ':' ||
        !IsDigit(s[i + 3]) || !IsDigit(s[i + 4]) || s[i + 5] != ':' || !IsDigit(s[i + 6]) || !IsDigit(s[i + 7])) {
        return false;
    }
    hour = (s[i] - '0') * 10 + (s[i + 1] - '0');
    min = (s[i + 3] - '0') * 10 + (s[i + 4] - '0');
    sec = (s[i + 6] - '0') * 10 + (s[i + 7] - '0');
    i += 8;
    SkipFractional(s, i);
    if (!ParseTimeZone(s, i, offsetSec)) {
        return false;
    }
    if (i != s.size()) {
        return false;
    }

    outTm.tm_year = year - 1900;
    outTm.tm_mon = mon - 1;
    outTm.tm_mday = day;
    outTm.tm_hour = hour;
    outTm.tm_min = min;
    outTm.tm_sec = sec;
    outTm.tm_isdst = 0;
    return true;
}

std::time_t TimeGmPortable(std::tm* tmUtc) {
#if defined(_WIN32)
    return _mkgmtime(tmUtc);
#else
    return timegm(tmUtc);
#endif
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
    // Portable: no leading-zero day on Windows use %#d; POSIX %-d.
#if defined(_WIN32)
    const size_t n = std::strftime(buf, sizeof(buf), "%b %#d '%y", &tmLocal);
#else
    const size_t n = std::strftime(buf, sizeof(buf), "%b %-d '%y", &tmLocal);
#endif
    if (n == 0) {
        return std::string();
    }
    return std::string(buf, n);
}

} // namespace

std::string FormatCompactJiraDateForDisplay(const std::string& raw) {
    const std::string s = TrimCopy(raw);
    if (s.empty()) {
        return raw;
    }

    std::tm wall{};
    int offsetSec = 0;
    if (!ParseJiraDateTime(s, wall, offsetSec)) {
        return raw;
    }

    const std::time_t wallAsUtcGuess = TimeGmPortable(&wall);
    if (wallAsUtcGuess == static_cast<std::time_t>(-1)) {
        return raw;
    }

    // Wall time in zone (offsetSec east of UTC) -> UTC epoch: subtract offset.
    const auto tp = std::chrono::system_clock::from_time_t(wallAsUtcGuess - static_cast<std::time_t>(offsetSec));
    const auto now = std::chrono::system_clock::now();
    const auto diffSec = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();

    constexpr long long kMinute = 60;
    constexpr long long kHour = 3600;
    constexpr long long kDay = 86400;
    constexpr long long kWeek = 604800;
    constexpr long long kThreeWeeks = 21 * kDay;

    const long long mag = (diffSec < 0) ? static_cast<long long>(-diffSec) : static_cast<long long>(diffSec);
    const char sign = (diffSec >= 0) ? '-' : '+';

    if (mag >= kThreeWeeks) {
        const std::string shortDate = FormatShortLocalDate(tp);
        return shortDate.empty() ? raw : shortDate;
    }

    // 7d .. <21d: whole weeks (floor), see plan.
    if (mag >= 7 * kDay) {
        const long long w = mag / kWeek;
        const long long weeks = std::max(1LL, w);
        std::ostringstream oss;
        oss << sign << weeks << 'w';
        return oss.str();
    }
    if (mag >= kDay) {
        const long long d = mag / kDay;
        std::ostringstream oss;
        oss << sign << d << 'd';
        return oss.str();
    }
    if (mag >= kHour) {
        const long long h = mag / kHour;
        std::ostringstream oss;
        oss << sign << h << 'h';
        return oss.str();
    }
    // < 60 minutes: minutes (at least 1m if non-zero).
    if (mag >= kMinute) {
        const long long m = mag / kMinute;
        std::ostringstream oss;
        oss << sign << m << 'm';
        return oss.str();
    }
    if (mag > 0) {
        std::ostringstream oss;
        oss << sign << 1 << 'm';
        return oss.str();
    }
    return std::string("0m");
}
