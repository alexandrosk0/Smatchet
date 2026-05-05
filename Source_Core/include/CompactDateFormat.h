#pragma once

#include <string>

/** Parsed wall date/time from Jira-style ISO strings (see TryParseJiraDateTime). */
struct ParsedJiraDateTime {
    int Year = 0;
    int Month = 0; // 1-12
    int Day = 0;
    int Hour = 0;
    int Minute = 0;
    int Second = 0;
    /** Seconds east of UTC (+01:00 -> 3600). Meaningful when HasTimeZoneSuffix is true, or 0 for Z. */
    int OffsetSec = 0;
    /** False: date-only (YYYY-MM-DD). True: includes T and time components. */
    bool HasWallTime = false;
    /** True if the string ended with Z or a numeric timezone offset. */
    bool HasTimeZoneSuffix = false;
    /** True if the suffix was the letter Z (offset is 0). */
    bool TimeZoneWasZ = false;
};

/** Parse Jira / ISO-8601-like date or datetime (supports trailing Z or ±HHMM / ±HH:MM). */
bool TryParseJiraDateTime(const std::string& raw, ParsedJiraDateTime& out);

/**
 * Format for Jira REST field update: date -> "YYYY-MM-DD"; datetime -> "YYYY-MM-DDTHH:MM:SS.000Z"
 * or with preserved offset suffix when HasTimeZoneSuffix is true.
 */
std::string FormatJiraDateOrDateTimeForApi(bool isDateField, const ParsedJiraDateTime& in);

/** Compact display for Jira ISO date/datetime strings (grid/tooltips use raw elsewhere). */
std::string FormatCompactJiraDateForDisplay(const std::string& raw);






