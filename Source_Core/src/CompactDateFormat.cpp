#include "CompactDateFormat.h"

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

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Parse optional timezone: Z | ±HHMM | ±HH:MM.
bool ParseTimeZone(const std::string& s, size_t& i, int& offsetSec, bool& outWasZ) {
    offsetSec = 0;
    outWasZ = false;
    if (i >= s.size()) {
        return true;
    }
    if (s[i] == 'Z' || s[i] == 'z') {
        ++i;
        outWasZ = true;
        offsetSec = 0;
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

void SkipFractional(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '.') {
        return;
    }
    ++i;
    while (i < s.size() && IsDigit(s[i])) {
        ++i;
    }
}

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

void AppendOffsetJira(std::string& out, int offsetSec) {
    const char sign = offsetSec >= 0 ? '+' : '-';
    int a = offsetSec >= 0 ? offsetSec : -offsetSec;
    const int hh = a / 3600;
    const int mm = (a % 3600) / 60;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d%02d", sign, hh, mm);
    out += buf;
}

} // namespace

bool TryParseJiraDateTime(const std::string& raw, ParsedJiraDateTime& out) {
    const std::string s = TrimCopy(raw);
    out = ParsedJiraDateTime();
    if (s.size() < 10) {
        return false;
    }
    if (!IsDigit(s[0]) || !IsDigit(s[1]) || !IsDigit(s[2]) || !IsDigit(s[3]) || s[4] != '-' || !IsDigit(s[5]) ||
        !IsDigit(s[6]) || s[7] != '-' || !IsDigit(s[8]) || !IsDigit(s[9])) {
        return false;
    }
    const int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    const int mon = (s[5] - '0') * 10 + (s[6] - '0');
    const int day = (s[8] - '0') * 10 + (s[9] - '0');
    size_t i = 10;

    out.Year = year;
    out.Month = mon;
    out.Day = day;

    if (i >= s.size() || s[i] != 'T') {
        out.HasWallTime = false;
        out.Hour = 0;
        out.Minute = 0;
        out.Second = 0;
        if (i < s.size()) {
            const size_t iBeforeTz = i;
            bool wasZ = false;
            if (!ParseTimeZone(s, i, out.OffsetSec, wasZ)) {
                return false;
            }
            out.HasTimeZoneSuffix = (i > iBeforeTz);
            out.TimeZoneWasZ = wasZ;
        }
        return i == s.size();
    }

    ++i;
    if (i + 8 > s.size() || !IsDigit(s[i]) || !IsDigit(s[i + 1]) || s[i + 2] != ':' || !IsDigit(s[i + 3]) ||
        !IsDigit(s[i + 4]) || s[i + 5] != ':' || !IsDigit(s[i + 6]) || !IsDigit(s[i + 7])) {
        return false;
    }
    out.Hour = (s[i] - '0') * 10 + (s[i + 1] - '0');
    out.Minute = (s[i + 3] - '0') * 10 + (s[i + 4] - '0');
    out.Second = (s[i + 6] - '0') * 10 + (s[i + 7] - '0');
    i += 8;
    out.HasWallTime = true;
    SkipFractional(s, i);
    const size_t iBeforeTz = i;
    bool wasZ = false;
    if (!ParseTimeZone(s, i, out.OffsetSec, wasZ)) {
        return false;
    }
    if (i != s.size()) {
        return false;
    }
    out.HasTimeZoneSuffix = (i > iBeforeTz);
    out.TimeZoneWasZ = wasZ;
    return true;
}

std::string FormatJiraDateOrDateTimeForApi(bool isDateField, const ParsedJiraDateTime& in) {
    char dateBuf[16];
    if (std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", in.Year, in.Month, in.Day) <= 0) {
        return std::string();
    }
    if (isDateField) {
        return std::string(dateBuf);
    }
    if (!in.HasWallTime) {
        return std::string(dateBuf);
    }
    char dateTimeBuf[32];
    if (std::snprintf(dateTimeBuf, sizeof(dateTimeBuf), "%sT%02d:%02d:%02d.000", dateBuf, in.Hour, in.Minute,
                      in.Second) <= 0) {
        return std::string();
    }
    std::string result(dateTimeBuf);
    if (!in.HasTimeZoneSuffix) {
        result += 'Z';
    } else if (in.TimeZoneWasZ) {
        result += 'Z';
    } else {
        AppendOffsetJira(result, in.OffsetSec);
    }
    return result;
}

std::string FormatCompactJiraDateForDisplay(const std::string& raw) {
    const std::string s = TrimCopy(raw);
    if (s.empty()) {
        return raw;
    }

    ParsedJiraDateTime p;
    if (!TryParseJiraDateTime(s, p)) {
        return raw;
    }

    std::tm wall{};
    if (!ParsedToTm(p, wall)) {
        return raw;
    }

    const int offsetSec = p.HasTimeZoneSuffix ? p.OffsetSec : 0;

    const std::time_t wallAsUtcGuess = TimeGmPortable(&wall);
    if (wallAsUtcGuess == static_cast<std::time_t>(-1)) {
        return raw;
    }

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

    if (mag >= 7 * kDay) {
        const long long w = mag / kWeek;
        const long long weeks = (std::max)(1LL, w);
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






