#include <doctest/doctest.h>

#include "TrackerDateTimePure.h"

#include <string>

using TrackerDateTimePure::ClampDayToMonth;
using TrackerDateTimePure::DaysInMonth;
using TrackerDateTimePure::DecMonth;
using TrackerDateTimePure::FirstOfMonthWeekday0Sun;
using TrackerDateTimePure::FormatFriendlyDate;
using TrackerDateTimePure::FormatFriendlyTime;
using TrackerDateTimePure::IncMonth;
using TrackerDateTimePure::IsLeapYear;
using TrackerDateTimePure::ParseFriendlyDate;
using TrackerDateTimePure::ParseFriendlyTime;
using TrackerDateTimePure::TodayUtcParsed;

TEST_CASE("IsLeapYear + DaysInMonth — Gregorian rules + out-of-range fallback") {
    SUBCASE("leap-year predicate") {
        CHECK(IsLeapYear(2000));       // /400
        CHECK(IsLeapYear(2024));       // /4 and not /100
        CHECK_FALSE(IsLeapYear(1900)); // /100 but not /400
        CHECK_FALSE(IsLeapYear(2023));
        CHECK_FALSE(IsLeapYear(2100));
    }
    SUBCASE("DaysInMonth — February honours leap year") {
        CHECK(DaysInMonth(2024, 2) == 29);
        CHECK(DaysInMonth(2023, 2) == 28);
        CHECK(DaysInMonth(2000, 2) == 29);
        CHECK(DaysInMonth(1900, 2) == 28);
    }
    SUBCASE("DaysInMonth — 30/31 day months") {
        CHECK(DaysInMonth(2024, 1) == 31);
        CHECK(DaysInMonth(2024, 4) == 30);
        CHECK(DaysInMonth(2024, 12) == 31);
    }
    SUBCASE("DaysInMonth — out-of-range month returns 31 (legacy widget fallback)") {
        CHECK(DaysInMonth(2024, 0) == 31);
        CHECK(DaysInMonth(2024, 13) == 31);
        CHECK(DaysInMonth(2024, -1) == 31);
    }
}

TEST_CASE("DecMonth + IncMonth — wrap across year boundary") {
    int y = 2026;
    int m = 5;
    SUBCASE("DecMonth within year") {
        DecMonth(y, m);
        CHECK(y == 2026);
        CHECK(m == 4);
    }
    SUBCASE("DecMonth wraps January -> December of prior year") {
        m = 1;
        DecMonth(y, m);
        CHECK(y == 2025);
        CHECK(m == 12);
    }
    SUBCASE("IncMonth within year") {
        IncMonth(y, m);
        CHECK(y == 2026);
        CHECK(m == 6);
    }
    SUBCASE("IncMonth wraps December -> January of next year") {
        m = 12;
        IncMonth(y, m);
        CHECK(y == 2027);
        CHECK(m == 1);
    }
}

TEST_CASE("FirstOfMonthWeekday0Sun — known anchors + crash-safety on bad inputs") {
    // 2024-01-01 was a Monday (UTC). Verified against `cal` / std::mktime.
    CHECK(FirstOfMonthWeekday0Sun(2024, 1) == 1);
    // 2026-05-01 was a Friday (UTC).
    CHECK(FirstOfMonthWeekday0Sun(2026, 5) == 5);
    // 2000-01-01 was a Saturday (UTC).
    CHECK(FirstOfMonthWeekday0Sun(2000, 1) == 6);
    // Pillar 3: malformed inputs must not crash; widget treats sentinel 0 as Sunday fallback.
    const int badYear = FirstOfMonthWeekday0Sun(-9999, 1);
    CHECK(badYear >= 0);
    CHECK(badYear <= 6);
    const int badMonth = FirstOfMonthWeekday0Sun(2024, 13);
    CHECK(badMonth >= 0);
    CHECK(badMonth <= 6);
}

TEST_CASE("TodayUtcParsed — shape contract (no wall-clock pinning)") {
    SUBCASE("date-only request") {
        const ParsedJiraDateTime p = TodayUtcParsed(false);
        CHECK(p.Year >= 1970);
        CHECK(p.Month >= 1);
        CHECK(p.Month <= 12);
        CHECK(p.Day >= 1);
        CHECK(p.Day <= 31);
        CHECK_FALSE(p.HasWallTime);
        CHECK(p.Hour == 0);
        CHECK(p.Minute == 0);
        CHECK(p.Second == 0);
        CHECK_FALSE(p.HasTimeZoneSuffix);
        CHECK(p.OffsetSec == 0);
    }
    SUBCASE("datetime request — wall time populated") {
        const ParsedJiraDateTime p = TodayUtcParsed(true);
        CHECK(p.HasWallTime);
        CHECK(p.Hour >= 0);
        CHECK(p.Hour <= 23);
        CHECK(p.Minute >= 0);
        CHECK(p.Minute <= 59);
        CHECK(p.Second >= 0);
        CHECK(p.Second <= 60); // tolerate leap-second tick
    }
}

TEST_CASE("ClampDayToMonth — boundary clamp + Feb leap-year") {
    ParsedJiraDateTime p{};
    SUBCASE("Day past end of month clamps to last day") {
        p.Day = 31;
        ClampDayToMonth(p, 2024, 4); // April: 30 days
        CHECK(p.Day == 30);
    }
    SUBCASE("Day below 1 clamps to 1") {
        p.Day = 0;
        ClampDayToMonth(p, 2024, 1);
        CHECK(p.Day == 1);
        p.Day = -5;
        ClampDayToMonth(p, 2024, 1);
        CHECK(p.Day == 1);
    }
    SUBCASE("Feb 29 valid in leap year, clamped to 28 otherwise") {
        p.Day = 29;
        ClampDayToMonth(p, 2024, 2); // leap
        CHECK(p.Day == 29);
        p.Day = 29;
        ClampDayToMonth(p, 2023, 2); // non-leap
        CHECK(p.Day == 28);
    }
    SUBCASE("In-range day untouched") {
        p.Day = 15;
        ClampDayToMonth(p, 2024, 6);
        CHECK(p.Day == 15);
    }
}

TEST_CASE("FormatFriendlyDate / ParseFriendlyDate — round-trip + range gates") {
    SUBCASE("Format M/D/YYYY") {
        ParsedJiraDateTime p{};
        p.Year = 2026;
        p.Month = 5;
        p.Day = 16;
        CHECK(FormatFriendlyDate(p) == "5/16/2026");
    }
    SUBCASE("Format pads year to 4 digits but not M/D") {
        ParsedJiraDateTime p{};
        p.Year = 99;
        p.Month = 1;
        p.Day = 2;
        CHECK(FormatFriendlyDate(p) == "1/2/0099");
    }
    SUBCASE("Parse accepts valid M/D/YYYY") {
        int y = 0, m = 0, d = 0;
        CHECK(ParseFriendlyDate("12/31/2030", y, m, d));
        CHECK(y == 2030);
        CHECK(m == 12);
        CHECK(d == 31);
    }
    SUBCASE("Parse rejects month > 12") {
        int y = 9999, m = 9999, d = 9999;
        CHECK_FALSE(ParseFriendlyDate("13/1/2026", y, m, d));
        // Pillar 3: rejection must leave outputs untouched (no garbage on failure).
        CHECK(y == 9999);
        CHECK(m == 9999);
        CHECK(d == 9999);
    }
    SUBCASE("Parse rejects day > 31") {
        int y = 0, m = 0, d = 0;
        CHECK_FALSE(ParseFriendlyDate("5/32/2026", y, m, d));
    }
    SUBCASE("DR32: Parse rejects days beyond the month's real length") {
        int y = 9999, m = 9999, d = 9999;
        CHECK_FALSE(ParseFriendlyDate("2/31/2026", y, m, d));  // Feb never has 31
        CHECK_FALSE(ParseFriendlyDate("2/29/2026", y, m, d));  // 2026 is not a leap year
        CHECK_FALSE(ParseFriendlyDate("4/31/2026", y, m, d));  // Apr has 30
        CHECK_FALSE(ParseFriendlyDate("6/31/2026", y, m, d));  // Jun has 30
        // rejection leaves outputs untouched
        CHECK(y == 9999);
        CHECK(m == 9999);
        CHECK(d == 9999);
    }
    SUBCASE("DR32: Parse accepts the real last day, incl. leap-year Feb 29") {
        int y = 0, m = 0, d = 0;
        CHECK(ParseFriendlyDate("2/28/2026", y, m, d));
        CHECK(ParseFriendlyDate("2/29/2024", y, m, d)); // 2024 is a leap year
        CHECK(ParseFriendlyDate("4/30/2026", y, m, d));
        CHECK(ParseFriendlyDate("12/31/2026", y, m, d));
    }
    SUBCASE("Parse rejects year < 1900 and > 3000") {
        int y = 0, m = 0, d = 0;
        CHECK_FALSE(ParseFriendlyDate("5/16/1899", y, m, d));
        CHECK_FALSE(ParseFriendlyDate("5/16/3001", y, m, d));
    }
    SUBCASE("Parse rejects malformed / empty") {
        int y = 0, m = 0, d = 0;
        CHECK_FALSE(ParseFriendlyDate("", y, m, d));
        CHECK_FALSE(ParseFriendlyDate("not a date", y, m, d));
        CHECK_FALSE(ParseFriendlyDate("5/16", y, m, d));
    }
    SUBCASE("Round-trip preserves Y/M/D for valid input") {
        ParsedJiraDateTime p{};
        p.Year = 2026;
        p.Month = 7;
        p.Day = 4;
        const std::string s = FormatFriendlyDate(p);
        int y = 0, m = 0, d = 0;
        REQUIRE(ParseFriendlyDate(s, y, m, d));
        CHECK(y == p.Year);
        CHECK(m == p.Month);
        CHECK(d == p.Day);
    }
}

TEST_CASE("FormatFriendlyTime / ParseFriendlyTime — 12-hour wrap + AM/PM + malformed") {
    SUBCASE("Format midnight as 12:MM AM") {
        ParsedJiraDateTime p{};
        p.Hour = 0;
        p.Minute = 5;
        CHECK(FormatFriendlyTime(p) == "12:05 AM");
    }
    SUBCASE("Format noon as 12:MM PM") {
        ParsedJiraDateTime p{};
        p.Hour = 12;
        p.Minute = 0;
        CHECK(FormatFriendlyTime(p) == "12:00 PM");
    }
    SUBCASE("Format PM hour subtracts 12") {
        ParsedJiraDateTime p{};
        p.Hour = 15;
        p.Minute = 30;
        CHECK(FormatFriendlyTime(p) == "03:30 PM");
    }
    SUBCASE("Parse 12-hour with AM/PM token") {
        int h = 0, m = 0;
        REQUIRE(ParseFriendlyTime("3:45 PM", h, m));
        CHECK(h == 15);
        CHECK(m == 45);
        REQUIRE(ParseFriendlyTime("12:00 AM", h, m));
        CHECK(h == 0);
        CHECK(m == 0);
        REQUIRE(ParseFriendlyTime("12:00 PM", h, m));
        CHECK(h == 12);
        CHECK(m == 0);
    }
    SUBCASE("Parse 24-hour fallback (no AM/PM)") {
        int h = 0, m = 0;
        REQUIRE(ParseFriendlyTime("23:59", h, m));
        CHECK(h == 23);
        CHECK(m == 59);
    }
    SUBCASE("Parse AM/PM token is case-insensitive") {
        int h = 0, m = 0;
        REQUIRE(ParseFriendlyTime("3:00 pm", h, m));
        CHECK(h == 15);
        REQUIRE(ParseFriendlyTime("3:00 Am", h, m));
        CHECK(h == 3);
    }
    SUBCASE("Parse rejects out-of-range") {
        int h = 0, m = 0;
        CHECK_FALSE(ParseFriendlyTime("24:00", h, m));
        CHECK_FALSE(ParseFriendlyTime("12:60 PM", h, m));
    }
    SUBCASE("Parse rejects malformed / empty — outputs untouched") {
        int h = 9999, m = 9999;
        CHECK_FALSE(ParseFriendlyTime("", h, m));
        CHECK_FALSE(ParseFriendlyTime("not a time", h, m));
        CHECK_FALSE(ParseFriendlyTime("12", h, m));
        CHECK(h == 9999);
        CHECK(m == 9999);
    }
}

// --- Gap map Tier 1 #4 — picker seed + Apply/Clear commit gate lifted from
// TrackerDateTimeFieldEditor.cpp (the untested editor TU). These pin the no-op-PUT rule:
// a re-Apply of an unchanged value (even when the wire form differs only in ms/seconds/zone
// spelling) and a Clear of an already-blank cell must never queue an edit.

TEST_CASE("InitDatePickerWorking — parseable / empty / unparseable seed modes") {
    using TrackerDateTimePure::InitDatePickerWorking;

    SUBCASE("parseable current value seeds working + view from the parse") {
        ParsedJiraDateTime working{};
        int viewYear = 0, viewMonth = 0;
        bool forceTextMode = true;
        InitDatePickerWorking("2026-03-05T10:20:30.000Z", /*isDateOnly=*/false, working, viewYear, viewMonth,
                              forceTextMode);
        CHECK_FALSE(forceTextMode);
        CHECK(working.Year == 2026);
        CHECK(working.Month == 3);
        CHECK(working.Day == 5);
        CHECK(viewYear == 2026);
        CHECK(viewMonth == 3);
    }
    SUBCASE("empty current value seeds today (UTC); wall time iff not date-only") {
        ParsedJiraDateTime working{};
        int viewYear = 0, viewMonth = 0;
        bool forceTextMode = true;
        InitDatePickerWorking("", /*isDateOnly=*/true, working, viewYear, viewMonth, forceTextMode);
        CHECK_FALSE(forceTextMode);
        CHECK(working.Year >= 2026); // "today" — sanity floor, not an exact clock assertion
        CHECK_FALSE(working.HasWallTime);
        CHECK(viewYear == working.Year);
        CHECK(viewMonth == working.Month);

        ParsedJiraDateTime workingDt{};
        bool forceTextModeDt = true;
        InitDatePickerWorking("", /*isDateOnly=*/false, workingDt, viewYear, viewMonth, forceTextModeDt);
        CHECK(workingDt.HasWallTime);
    }
    SUBCASE("non-empty unparseable value flips text mode and touches nothing else") {
        ParsedJiraDateTime working{};
        working.Year = 1234; // sentinels prove the outputs stay untouched
        int viewYear = -7, viewMonth = -8;
        bool forceTextMode = false;
        InitDatePickerWorking("not a date", /*isDateOnly=*/false, working, viewYear, viewMonth, forceTextMode);
        CHECK(forceTextMode);
        CHECK(working.Year == 1234);
        CHECK(viewYear == -7);
        CHECK(viewMonth == -8);
    }
}

TEST_CASE("PlanDateTimeCommit — Apply/Clear gate PUTs on real change only") {
    using TrackerDateTimePure::DateTimeCommitPlan;
    using TrackerDateTimePure::PlanDateTimeCommit;

    SUBCASE("neither pressed never queues") {
        ParsedJiraDateTime working{};
        const DateTimeCommitPlan plan = PlanDateTimeCommit(false, false, true, working, "2026-01-01");
        CHECK_FALSE(plan.Queue);
    }
    SUBCASE("Apply with a real change queues the canonical value") {
        ParsedJiraDateTime working{};
        REQUIRE(TryParseJiraDateTime("2026-03-05", working));
        const DateTimeCommitPlan plan = PlanDateTimeCommit(true, false, /*isDateOnly=*/true, working, "2026-03-04");
        REQUIRE(plan.Queue);
        REQUIRE(plan.Values.size() == 1);
        CHECK(plan.Values[0] == "2026-03-05");
    }
    SUBCASE("re-Apply of an unchanged value is a no-op even when the wire form differs") {
        // The stored wire value carries non-zero milliseconds + the +0000 offset spelling; the
        // picker's working copy is the parse of that same instant. Raw-string comparison would
        // read "changed" (".123" vs ".000"); the canonical-form comparison must not.
        const std::string wire = "2026-03-05T10:00:00.123+0000";
        ParsedJiraDateTime working{};
        REQUIRE(TryParseJiraDateTime(wire, working));
        const DateTimeCommitPlan plan = PlanDateTimeCommit(true, false, /*isDateOnly=*/false, working, wire);
        CHECK_FALSE(plan.Queue);
    }
    SUBCASE("Apply clamps an out-of-range day before formatting (in-place, like the widget)") {
        ParsedJiraDateTime working{};
        REQUIRE(TryParseJiraDateTime("2026-02-10", working));
        working.Day = 31; // month navigation can leave a day past the target month's end
        const DateTimeCommitPlan plan = PlanDateTimeCommit(true, false, /*isDateOnly=*/true, working, "");
        REQUIRE(plan.Queue);
        CHECK(plan.Values[0] == "2026-02-28"); // 2026 is not a leap year
        CHECK(working.Day == 28);              // clamp mutates the working copy, byte-identical to the original
    }
    SUBCASE("Clear on an already-blank cell never queues; Clear on a populated cell queues empty") {
        ParsedJiraDateTime working{};
        const DateTimeCommitPlan blankPlan = PlanDateTimeCommit(false, true, true, working, "   ");
        CHECK_FALSE(blankPlan.Queue);
        const DateTimeCommitPlan popPlan = PlanDateTimeCommit(false, true, true, working, "2026-03-04");
        REQUIRE(popPlan.Queue);
        CHECK(popPlan.Values.empty());
    }
    SUBCASE("unparseable current value is conservatively treated as changed") {
        ParsedJiraDateTime working{};
        REQUIRE(TryParseJiraDateTime("2026-03-05", working));
        const DateTimeCommitPlan plan =
            PlanDateTimeCommit(true, false, /*isDateOnly=*/true, working, "not a date at all");
        CHECK(plan.Queue);
    }
}
