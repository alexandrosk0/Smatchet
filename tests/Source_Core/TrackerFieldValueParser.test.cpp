#include <doctest/doctest.h>

#include "TrackerFieldValueParser.h"

#include <string>

TEST_CASE("ParseWorkDurationToSeconds handles empty + zero input") {
    CHECK(ParseWorkDurationToSeconds("") == 0);
    CHECK(ParseWorkDurationToSeconds("   ") == 0);
    CHECK(ParseWorkDurationToSeconds("0") == 0);
}

TEST_CASE("ParseWorkDurationToSeconds treats all-digits as seconds") {
    CHECK(ParseWorkDurationToSeconds("60") == 60);
    CHECK(ParseWorkDurationToSeconds("3600") == 3600);
    CHECK(ParseWorkDurationToSeconds("99999") == 99999);
}

TEST_CASE("ParseWorkDurationToSeconds parses single-unit suffixes") {
    CHECK(ParseWorkDurationToSeconds("1m") == 60);
    CHECK(ParseWorkDurationToSeconds("30m") == 30 * 60);
    CHECK(ParseWorkDurationToSeconds("1h") == 3600);
    CHECK(ParseWorkDurationToSeconds("2h") == 2 * 3600);
    CHECK(ParseWorkDurationToSeconds("1d") == 8 * 3600);
    CHECK(ParseWorkDurationToSeconds("1w") == 5 * 8 * 3600);
}

TEST_CASE("ParseWorkDurationToSeconds is case-insensitive on unit char") {
    CHECK(ParseWorkDurationToSeconds("1H") == 3600);
    CHECK(ParseWorkDurationToSeconds("30M") == 1800);
    CHECK(ParseWorkDurationToSeconds("1W") == 5 * 8 * 3600);
    CHECK(ParseWorkDurationToSeconds("1D") == 8 * 3600);
}

TEST_CASE("ParseWorkDurationToSeconds composes multi-unit tokens") {
    CHECK(ParseWorkDurationToSeconds("1h 30m") == 3600 + 1800);
    CHECK(ParseWorkDurationToSeconds("2d 3h") == 2 * 8 * 3600 + 3 * 3600);
    CHECK(ParseWorkDurationToSeconds("1w 2d 3h 15m") == 5 * 8 * 3600 + 2 * 8 * 3600 + 3 * 3600 + 15 * 60);
}

TEST_CASE("ParseWorkDurationToSeconds stops at first unknown unit") {
    // "1.5h" — '.' is not a valid unit, parse stops; nothing was accumulated.
    CHECK(ParseWorkDurationToSeconds("1.5h") == 0);
    // "1x" — unit 'x' unrecognised, no partial accumulation.
    CHECK(ParseWorkDurationToSeconds("1x") == 0);
}

TEST_CASE("FormatWorkDurationFromSeconds returns empty for non-positive input") {
    CHECK(FormatWorkDurationFromSeconds(0) == "");
    CHECK(FormatWorkDurationFromSeconds(-1) == "");
    CHECK(FormatWorkDurationFromSeconds(-3600) == "");
}

TEST_CASE("FormatWorkDurationFromSeconds emits Jira-style units (w/d/h/m)") {
    CHECK(FormatWorkDurationFromSeconds(3600) == "1h");
    CHECK(FormatWorkDurationFromSeconds(2 * 3600) == "2h");
    CHECK(FormatWorkDurationFromSeconds(8 * 3600) == "1d");
    CHECK(FormatWorkDurationFromSeconds(5 * 8 * 3600) == "1w");
    CHECK(FormatWorkDurationFromSeconds(3600 + 1800) == "1h 30m");
}

TEST_CASE("FormatWorkDurationFromSeconds always emits an hours bucket when sub-hour") {
    // The formatter forces "0h" when no weeks/days exist so a sub-hour duration
    // is still readable as a Jira duration. Guards against display regressions.
    CHECK(FormatWorkDurationFromSeconds(60) == "0h 1m");
    CHECK(FormatWorkDurationFromSeconds(30 * 60) == "0h 30m");
}

TEST_CASE("Parse + Format round-trip preserves whole-unit durations") {
    const long long inputs[] = {
        60, 30 * 60, 3600, 2 * 3600 + 15 * 60, 8 * 3600, 5 * 8 * 3600, 5 * 8 * 3600 + 2 * 8 * 3600 + 3 * 3600 + 15 * 60,
    };
    for (long long seconds : inputs) {
        const std::string formatted = FormatWorkDurationFromSeconds(seconds);
        const long long reparsed = ParseWorkDurationToSeconds(formatted);
        CHECK(reparsed == seconds);
    }
}
