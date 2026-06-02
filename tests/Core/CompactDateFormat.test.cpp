#include <doctest/doctest.h>

#include "CompactDateFormat.h"

#include <string>

// Coverage for FormatCompactJiraDateForDisplay's deterministic dispatch paths
// (parse-failure passthrough + absolute_iso / absolute_friendly), captured as
// goldens against current behaviour during the function-size decomposition that
// lifted ParseDisplayTimePointCached / FormatRelativeMagnitude / ComputeCompactJiraDisplay
// out of the monolith. The relative-magnitude path ("-3d", "+2w", ...) reads the
// wall clock with no injectable "now", so it is exercised only for its non-empty
// shape, not an exact value.
//
// CompactDateFormat.cpp is already linked into the doctest rig (tests/CMakeLists.txt).

TEST_CASE("FormatCompactJiraDateForDisplay — unparseable input passes through unchanged") {
    CHECK(FormatCompactJiraDateForDisplay("not a date", "compact", 21) == "not a date");
    CHECK(FormatCompactJiraDateForDisplay("", "compact", 21) == "");
    CHECK(FormatCompactJiraDateForDisplay("2026-13-99", "absolute_iso", 21) == "2026-13-99");
    // Distinct raw strings avoid the per-string format cache returning a stale value.
    CHECK(FormatCompactJiraDateForDisplay("garbage-xyz", "absolute_friendly", 21) == "garbage-xyz");
}

TEST_CASE("FormatCompactJiraDateForDisplay — absolute_iso renders local Y-M-D[ H:M:S]") {
    // Date-only input -> date-only ISO. The value is parsed as UTC midnight and
    // rendered in local time, so the day can shift by ±1 depending on the runner's
    // timezone (e.g. "2026-03-15" -> "2026-03-14" under UTC-offset zones). Assert the
    // stable shape + year-month rather than a TZ-dependent exact day.
    const std::string dateOnly = FormatCompactJiraDateForDisplay("2026-03-15", "absolute_iso", 21);
    REQUIRE(dateOnly.size() == 10); // "YYYY-MM-DD"
    CHECK(dateOnly[4] == '-');
    CHECK(dateOnly[7] == '-');
    CHECK(dateOnly.substr(0, 7) == "2026-03");
    CHECK((dateOnly == "2026-03-15" || dateOnly == "2026-03-14"));

    // Datetime with explicit Z. The displayed value is local, so we assert the
    // stable shape (length + separators) rather than a TZ-dependent literal.
    const std::string dt = FormatCompactJiraDateForDisplay("2026-03-15T08:30:00Z", "absolute_iso", 21);
    REQUIRE(dt.size() == 19); // "YYYY-MM-DD HH:MM:SS"
    CHECK(dt[4] == '-');
    CHECK(dt[7] == '-');
    CHECK(dt[10] == ' ');
    CHECK(dt[13] == ':');
    CHECK(dt[16] == ':');
    CHECK(dt.substr(0, 4) == "2026");
}

TEST_CASE("FormatCompactJiraDateForDisplay — absolute_friendly renders a Mon DD, YYYY label") {
    const std::string d = FormatCompactJiraDateForDisplay("2026-07-04", "absolute_friendly", 21);
    // "Jul 04, 2026" — month abbrev + ", YYYY".
    CHECK(d.find("2026") != std::string::npos);
    CHECK(d.find(',') != std::string::npos);
    CHECK(d.substr(0, 3) == "Jul");

    const std::string dt = FormatCompactJiraDateForDisplay("2026-07-04T13:45:00Z", "absolute_friendly", 21);
    CHECK(dt.find("2026") != std::string::npos);
    CHECK(dt.find(',') != std::string::npos);
}

TEST_CASE("FormatCompactJiraDateForDisplay — relative path yields a non-empty signed magnitude label") {
    // No injectable clock, so assert only the invariant shape: a sign followed by
    // a magnitude+unit. Only formatOption == "compact" engages the short-date
    // threshold branch; the "relative" option always stays on the magnitude
    // formatter regardless of threshold, so an ancient date yields e.g. "-26y".
    const std::string rel = FormatCompactJiraDateForDisplay("2000-01-01T00:00:00Z", "relative", 21);
    REQUIRE_FALSE(rel.empty());
    CHECK((rel[0] == '-' || rel[0] == '+'));
    // Ends in a unit char from the {y,o,w,d,h,m} set.
    const char last = rel[rel.size() - 1];
    CHECK((last == 'y' || last == 'o' || last == 'w' || last == 'd' || last == 'h' || last == 'm'));
}
