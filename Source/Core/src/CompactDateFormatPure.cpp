// CompactDateFormatPure — the cpr/ImGui-free half of CompactDateFormat.
//
// `TryParseJiraDateTime` + `FormatJiraDateOrDateTimeForApi` are pure string<->date
// helpers used by the field-edit / create payload path (TrackerFieldPayloadPure).
// They were split out of CompactDateFormat.cpp so they link into the headless Linux
// ThreadSanitizer subset (`SmatchetTsanTests`): the display-side
// `FormatCompactJiraDateForDisplay` needs <imgui.h> (ImGui::GetTime / GetFrameCount
// for its per-frame relative-time cache), which is unavailable on that target.
// Same decouple pattern as TrackerHttpPure (#1339). Declarations stay in the shared
// CompactDateFormat.h, so callers are unaffected and the definitions just live here.

#include "CompactDateFormat.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace {

// Duplicated from CompactDateFormat.cpp (anonymous-namespace, internal linkage —
// no ODR conflict). FormatCompactJiraDateForDisplay still needs its own copy.
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
