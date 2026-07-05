#ifndef SMATCHET_TRACKER_DATETIME_PURE_H
#define SMATCHET_TRACKER_DATETIME_PURE_H

#include "CompactDateFormat.h"

#include <ctime>
#include <string>
#include <vector>

/**
 * Pure-logic date helpers lifted from TrackerDateTimeFieldEditor.cpp's anonymous namespace so
 * they can be unit-tested without dragging in ImGui (the production TU also hosts the calendar
 * widget which pulls `<imgui.h>`). No GUI, no I/O, no SQLite, no cpr. Implementations are
 * byte-identical to the originals (arithmetic + control flow preserved); the only change is
 * namespace + linkage.
 *
 * Owner: `tracker-backend` / `grid-engine` subsystem split. Test surface:
 * tests/Core/TrackerDateTimePure.test.cpp.
 */
namespace TrackerDateTimePure {

/** Cross-platform `timegm` wrapper. Uses `_mkgmtime` on Windows, `timegm` on POSIX. */
std::time_t TimeGmPortable(std::tm* tmUtc);

/** Proleptic Gregorian leap-year predicate. */
bool IsLeapYear(int y);

/**
 * Days in `month` of `year`. `month` is 1-12; out-of-range months return 31 (legacy widget
 * fallback — kept byte-identical so the calendar grid never under-allocates rows).
 */
int DaysInMonth(int year, int month);

/** Decrement `month` (1-12); rolls back to December of `year - 1` when month drops below 1. */
void DecMonth(int& year, int& month);

/** Increment `month` (1-12); rolls forward to January of `year + 1` when month rises above 12. */
void IncMonth(int& year, int& month);

/**
 * Weekday-of-1st-of-month in UTC, 0 = Sunday .. 6 = Saturday. Returns 0 (Sunday fallback) on
 * `mktime`/`gmtime` failure — same legacy fallback the calendar widget relies on.
 */
int FirstOfMonthWeekday0Sun(int year, int month);

/**
 * Current UTC wall-time wrapped in a ParsedJiraDateTime. `includeTime = false` zeros the
 * H/M/S fields and clears `HasWallTime`. `HasTimeZoneSuffix` is always false (caller decides
 * how to format).
 */
ParsedJiraDateTime TodayUtcParsed(bool includeTime);

/**
 * Clamp `w.Day` to `[1, DaysInMonth(year, month)]` in-place. Used by the calendar widget when
 * the user navigates months and the current day would fall off the new month's last row.
 */
void ClampDayToMonth(ParsedJiraDateTime& w, int year, int month);

/** Format `M/D/YYYY` (no zero-padding on month / day). Used by the friendly-input row. */
std::string FormatFriendlyDate(const ParsedJiraDateTime& p);

/**
 * Parse `M/D/YYYY`. Accepts 1-12 / 1-31 / 1900-3000; rejects anything else. Returns false
 * without touching outputs on parse miss. Year overflow / negative inputs are rejected
 * because `sscanf` accepts them but the explicit range gate rejects them.
 */
bool ParseFriendlyDate(const std::string& s, int& outY, int& outM, int& outD);

/**
 * Format `HH:MM AM/PM` from a ParsedJiraDateTime (hours / minutes only — seconds dropped).
 * Hour 0 -> 12 AM; hour 12 -> 12 PM; 13-23 -> 1-11 PM.
 */
std::string FormatFriendlyTime(const ParsedJiraDateTime& p);

/**
 * Parse `HH:MM AM/PM` or `HH:MM` (24-hour). AM/PM token case-insensitive. Rejects
 * out-of-range hour / minute. Returns false without touching outputs on parse miss.
 */
bool ParseFriendlyTime(const std::string& s, int& outH, int& outM);

/**
 * Seed the date-picker working state when an edit session starts (lifted byte-identical from
 * TrackerDateTimeFieldEditor.cpp's InitDatePickerWorking — gap map Tier 1 #4). Three modes:
 * parseable `currentValue` → working = parsed (day clamped to the view month); empty →
 * working = today (UTC; wall-time iff `!isDateOnly`); non-empty unparseable → only
 * `forceTextMode` flips true (working / view outputs untouched, matching the widget's
 * raw-ISO fallback branch).
 */
void InitDatePickerWorking(const std::string& currentValue, bool isDateOnly, ParsedJiraDateTime& working, int& viewYear,
                           int& viewMonth, bool& forceTextMode);

/** Decision output of PlanDateTimeCommit: whether to queue, and the values to queue. */
struct DateTimeCommitPlan {
    bool Queue = false;
    std::vector<std::string> Values; // empty vector = clear the field
};

/**
 * The Apply/Clear commit gate of the date-picker popup (lifted byte-identical from
 * TrackerDateTimeFieldEditor.cpp — gap map Tier 1 #4). Gates the PUT on a REAL change so a
 * re-Apply of an unchanged value or a Clear of an already-empty cell never fires a stray
 * no-op PUT: Apply clamps `working`'s day (in-place, matching the widget), formats it via
 * FormatJiraDateOrDateTimeForApi, and compares against the CANONICAL form of `currentValue`
 * (both sides through the same formatter, so ms/seconds/zone-spelling differences in the wire
 * value never read as a change; unparseable `currentValue` conservatively compares raw).
 * Clear queues an empty value-list iff the current value is not already blank.
 */
DateTimeCommitPlan PlanDateTimeCommit(bool applyPressed, bool clearPressed, bool isDateOnly,
                                      ParsedJiraDateTime& working, const std::string& currentValue);

} // namespace TrackerDateTimePure

#endif
