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
