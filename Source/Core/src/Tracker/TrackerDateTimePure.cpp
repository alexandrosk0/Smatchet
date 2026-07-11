#include "TrackerDateTimePure.h"

#include "TicketFieldEditorCommitPolicyPure.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace TrackerDateTimePure {

#if defined(_WIN32)
std::time_t TimeGmPortable(std::tm* tmUtc) { return _mkgmtime(tmUtc); }
#else
std::time_t TimeGmPortable(std::tm* tmUtc) { return timegm(tmUtc); }
#endif

bool IsLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    int d = kDays[month - 1];
    if (month == 2 && IsLeapYear(year)) {
        d = 29;
    }
    return d;
}

void DecMonth(int& year, int& month) {
    if (--month < 1) {
        month = 12;
        --year;
    }
}

void IncMonth(int& year, int& month) {
    if (++month > 12) {
        month = 1;
        ++year;
    }
}

int FirstOfMonthWeekday0Sun(int year, int month) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = 0;
    const std::time_t tt = TimeGmPortable(&t);
    if (tt == static_cast<std::time_t>(-1)) {
        return 0;
    }
    std::tm r{};
#if defined(_WIN32)
    if (gmtime_s(&r, &tt) != 0) {
        return 0;
    }
#else
    if (gmtime_r(&tt, &r) == nullptr) {
        return 0;
    }
#endif
    return r.tm_wday;
}

ParsedJiraDateTime TodayUtcParsed(bool includeTime) {
    ParsedJiraDateTime p{};
    const std::time_t now = std::time(nullptr);
    std::tm r{};
#if defined(_WIN32)
    if (gmtime_s(&r, &now) != 0) {
        return p;
    }
#else
    if (gmtime_r(&now, &r) == nullptr) {
        return p;
    }
#endif
    p.Year = r.tm_year + 1900;
    p.Month = r.tm_mon + 1;
    p.Day = r.tm_mday;
    p.HasWallTime = includeTime;
    p.Hour = includeTime ? r.tm_hour : 0;
    p.Minute = includeTime ? r.tm_min : 0;
    p.Second = includeTime ? r.tm_sec : 0;
    p.HasTimeZoneSuffix = false;
    p.TimeZoneWasZ = false;
    p.OffsetSec = 0;
    return p;
}

void ClampDayToMonth(ParsedJiraDateTime& w, int year, int month) {
    const int dim = DaysInMonth(year, month);
    if (w.Day > dim) {
        w.Day = dim;
    }
    if (w.Day < 1) {
        w.Day = 1;
    }
}

std::string FormatFriendlyDate(const ParsedJiraDateTime& p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d/%d/%04d", p.Month, p.Day, p.Year);
    return std::string(buf);
}

bool ParseFriendlyDate(const std::string& s, int& outY, int& outM, int& outD) {
    int m = 0, d = 0, y = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d", &m, &d, &y) == 3) {
        // Validate the day against the actual length of that month/year (leap years
        // included) rather than a blanket <= 31, so impossible dates like "2/31/2026"
        // are rejected here instead of being formatted and submitted to the tracker.
        if (m >= 1 && m <= 12 && y >= 1900 && y <= 3000 && d >= 1 && d <= DaysInMonth(y, m)) {
            outY = y;
            outM = m;
            outD = d;
            return true;
        }
    }
    return false;
}

std::string FormatFriendlyTime(const ParsedJiraDateTime& p) {
    int h = p.Hour;
    const char* ampm = "AM";
    if (h >= 12) {
        ampm = "PM";
        if (h > 12)
            h -= 12;
    }
    if (h == 0)
        h = 12;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d %s", h, p.Minute, ampm);
    return std::string(buf);
}

void InitDatePickerWorking(const std::string& currentValue, bool isDateOnly, ParsedJiraDateTime& working, int& viewYear,
                           int& viewMonth, bool& forceTextMode) {
    ParsedJiraDateTime parsed;
    if (TryParseJiraDateTime(currentValue, parsed)) {
        working = parsed;
        forceTextMode = false;
        viewYear = parsed.Year;
        viewMonth = parsed.Month;
        ClampDayToMonth(working, viewYear, viewMonth);
    } else if (currentValue.empty()) {
        working = TodayUtcParsed(!isDateOnly);
        forceTextMode = false;
        viewYear = working.Year;
        viewMonth = working.Month;
    } else {
        forceTextMode = true;
    }
}

DateTimeCommitPlan PlanDateTimeCommit(bool applyPressed, bool clearPressed, bool isDateOnly,
                                      ParsedJiraDateTime& working, const std::string& currentValue) {
    DateTimeCommitPlan plan;
    if (!applyPressed && !clearPressed) {
        return plan;
    }
    std::string canon;
    if (applyPressed) {
        ClampDayToMonth(working, working.Year, working.Month);
        canon = FormatJiraDateOrDateTimeForApi(isDateOnly, working);
    }
    const bool curBlank =
        std::all_of(currentValue.begin(), currentValue.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    // Compare canon against the canonical form of currentValue, not its raw wire string:
    // FormatJiraDateOrDateTimeForApi forces ".000" milliseconds and always emits seconds, so a
    // re-Apply of an unchanged value whose Jira wire form differs only in ms/seconds/zone
    // spelling (e.g. "...T10:00:00.123+0000") would otherwise read as changed and fire a stray
    // no-op PUT. Both sides go through the same formatter, so equality means semantic identity.
    // Unparseable currentValue falls back to the raw string (conservative — treats as changed).
    std::string canonCurrent = currentValue;
    ParsedJiraDateTime curParsed;
    if (TryParseJiraDateTime(currentValue, curParsed)) {
        canonCurrent = FormatJiraDateOrDateTimeForApi(isDateOnly, curParsed);
    }
    const bool valueChanged = clearPressed ? !curBlank : (canon != canonCurrent);
    if (TicketFieldEditorCommitPolicyPure::ShouldCommitTouchPopupEdit(/*savePressed=*/true, valueChanged)) {
        plan.Queue = true;
        if (!clearPressed) {
            plan.Values.push_back(canon);
        }
    }
    return plan;
}

bool ParseFriendlyTime(const std::string& s, int& outH, int& outM) {
    int h = 0, m = 0;
    char ampm[8] = "";
    if (std::sscanf(s.c_str(), "%d:%d %7s", &h, &m, ampm) == 3) {
        std::string ampmStr = ampm;
        std::transform(ampmStr.begin(), ampmStr.end(), ampmStr.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (ampmStr == "PM" && h < 12)
            h += 12;
        if (ampmStr == "AM" && h == 12)
            h = 0;
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            outH = h;
            outM = m;
            return true;
        }
    } else if (std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2) {
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            outH = h;
            outM = m;
            return true;
        }
    }
    return false;
}

} // namespace TrackerDateTimePure
