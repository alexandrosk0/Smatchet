#include <doctest/doctest.h>

// Coverage for the time-tracking sort path extracted from TicketGridModel.cpp
// (TEST_COVERAGE_GAP_MAP.md Tier 1 #1). These comparators run on the UI thread over
// tracker-supplied cell values, so the regression contract is: terminate on ANY input
// (CPP_CODE_AUDIT.md #3 — the pre-fix parser looped forever on a non-unit char like the
// '.' in "2.5h") and saturate instead of overflowing on hostile magnitudes (#19).
#include "TicketGridDurationSortPure.h"

#include <string>

using TicketGridDurationSortPure::CompareTimeTrackingValues;
using TicketGridDurationSortPure::MaxDurationSeconds;
using TicketGridDurationSortPure::ParseDurationToSecondsForSort;

TEST_CASE("whole-integer fast path parses plain seconds") {
    CHECK(ParseDurationToSecondsForSort("3600") == 3600);
    CHECK(ParseDurationToSecondsForSort("0") == 0);
    CHECK(ParseDurationToSecondsForSort("  1200  ") == 1200);
    CHECK(ParseDurationToSecondsForSort("\t42") == 42);
    // Negative plain seconds pass through the fast path untouched (sort just orders them).
    CHECK(ParseDurationToSecondsForSort("-60") == -60);
}

TEST_CASE("unit-by-unit parse handles Jira pretty-printed durations") {
    CHECK(ParseDurationToSecondsForSort("1h") == 3600);
    CHECK(ParseDurationToSecondsForSort("3h 30m") == 3 * 3600 + 30 * 60);
    // Jira working units: week = 5 days, day = 8 hours.
    CHECK(ParseDurationToSecondsForSort("1d") == 8 * 3600);
    CHECK(ParseDurationToSecondsForSort("1w") == 5 * 8 * 3600);
    CHECK(ParseDurationToSecondsForSort("1w 2d 3h 4m") == (5 * 8 + 2 * 8 + 3) * 3600 + 4 * 60);
    CHECK(ParseDurationToSecondsForSort("2H") == 2 * 3600);
    // No-space unit runs.
    CHECK(ParseDurationToSecondsForSort("3h30m") == 3 * 3600 + 30 * 60);
}

TEST_CASE("non-unit characters never stall the parser (CPP_CODE_AUDIT.md #3 regression)") {
    // Pre-fix, each of these hung the UI thread the instant the user sorted the column.
    // The exact totals are secondary; termination + monotone plausibility is the contract.
    CHECK(ParseDurationToSecondsForSort("2.5h") == 2 + 5 * 3600);
    CHECK(ParseDurationToSecondsForSort("3h 30m left") > 0);
    // A skipped non-unit char contributes its pending number (implicit 1) as seconds.
    CHECK(ParseDurationToSecondsForSort("(2h)") == 1 + 2 * 3600 + 1);
    CHECK(ParseDurationToSecondsForSort("~") == 1);
    CHECK(ParseDurationToSecondsForSort("a b c") == 3);
    CHECK(ParseDurationToSecondsForSort("30s") == 30);
    CHECK(ParseDurationToSecondsForSort("...") == 3);
}

TEST_CASE("hostile magnitudes saturate instead of overflowing (CPP_CODE_AUDIT.md #19)") {
    CHECK(ParseDurationToSecondsForSort("99999999999999w") == MaxDurationSeconds());
    CHECK(ParseDurationToSecondsForSort("9223372036854775807m") == MaxDurationSeconds());
    // Repeated additions saturate too, they don't wrap.
    CHECK(ParseDurationToSecondsForSort("9223372036854775806 9223372036854775806") ==
          MaxDurationSeconds());
    // Out-of-range digit run aborts the number parse without stalling.
    CHECK(ParseDurationToSecondsForSort("99999999999999999999h") == 0);
}

TEST_CASE("empty and whitespace-only input parses to zero") {
    CHECK(ParseDurationToSecondsForSort("") == 0);
    CHECK(ParseDurationToSecondsForSort("   ") == 0);
    CHECK(ParseDurationToSecondsForSort("\t \t") == 0);
}

TEST_CASE("CompareTimeTrackingValues orders by parsed seconds across formats") {
    CHECK(CompareTimeTrackingValues("30m", "1h") == -1);
    CHECK(CompareTimeTrackingValues("1h", "30m") == 1);
    CHECK(CompareTimeTrackingValues("1h", "3600") == 0);
    CHECK(CompareTimeTrackingValues("2d", "1d 8h") == 0);
    CHECK(CompareTimeTrackingValues("", "1m") == -1);
    CHECK(CompareTimeTrackingValues("garbage", "garbage") == 0);
}
