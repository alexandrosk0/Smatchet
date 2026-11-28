#include <doctest/doctest.h>

#include "SmatchetLocalization.h"
#include "Tracker/TrackerFieldValueParseExplain.h"

#include <cstddef>
#include <string>

using namespace TrackerParseExplain;

namespace {

// English is the fallback every string carries, so pin the text under en-US whatever the host config says.
struct EnglishGuard {
    EnglishGuard() { SmatchetLocalization::SetLanguage("en-US"); }
};

struct DurationCase {
    const char* Input;
    BareNumberUnit Bare;
    long long Seconds;
    const char* Canonical;
};

struct DurationRejectCase {
    const char* Input;
    ParseMessageId Id;
    const char* Arg;
};

DurationParse Dur(const char* text, BareNumberUnit bare = BareNumberUnit::Minutes) {
    return ParseDurationInput(text, bare);
}

CivilDate Today() { return CivilDate{2026, 11, 16}; } // a Monday

std::string Iso(const char* text, DateOrder order = DateOrder::MonthFirst) {
    const DateParse p = ParseDateInput(text, Today(), order);
    return p.Ok ? FormatIsoDate(p.Date) : std::string("<fail>");
}

} // namespace

TEST_CASE("ParseDurationInput accepts natural durations and commits the canonical form") {
    // The audit's own examples (#38) come first, then the shapes around them.
    const DurationCase kAccept[] = {
        {"1.5h", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {"90 m", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {"1h30", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {"1h 30", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {"1:30", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {"2h 30m", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"2h30m", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"2H 30M", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"  2h  30m  ", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"2h, 30m", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"2h and 30m", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"2h + 30m", BareNumberUnit::Minutes, 9000, "2h 30m"},
        {"1 hour 15 mins", BareNumberUnit::Minutes, 4500, "1h 15m"},
        {"2 hrs", BareNumberUnit::Minutes, 7200, "2h"},
        {"45 minutes", BareNumberUnit::Minutes, 2700, "45m"},
        {"2d 4h", BareNumberUnit::Minutes, 72000, "2d 4h"},
        {"1w", BareNumberUnit::Minutes, 144000, "1w"},
        {"1 week 2 days", BareNumberUnit::Minutes, 144000 + 2 * 28800, "1w 2d"},
        {"1w 2d 3h 4m", BareNumberUnit::Minutes, 144000 + 2 * 28800 + 3 * 3600 + 4 * 60, "1w 2d 3h 4m"},
        {"1,5h", BareNumberUnit::Minutes, 5400, "1h 30m"},
        {".5d", BareNumberUnit::Minutes, 14400, "4h"},
        {"0.5 d", BareNumberUnit::Minutes, 14400, "4h"},
        {"2.5h", BareNumberUnit::Minutes, 9000, "2h 30m"},
    };
    for (const DurationCase& c : kAccept) {
        INFO("input: " << std::string(c.Input));
        const DurationParse p = Dur(c.Input, c.Bare);
        CHECK(p.Ok);
        CHECK_FALSE(p.Empty);
        CHECK(p.Seconds == c.Seconds);
        CHECK(p.Canonical == c.Canonical);
        CHECK_FALSE(p.Message.IsError());
    }
}

TEST_CASE("ParseDurationInput: a bare whole number is read in the unit the caller names") {
    CHECK(Dur("90", BareNumberUnit::Minutes).Seconds == 5400);
    CHECK(Dur("90", BareNumberUnit::Minutes).Canonical == "1h 30m");
    CHECK(Dur("90", BareNumberUnit::Hours).Seconds == 90 * 3600);
    CHECK(Dur("90", BareNumberUnit::Hours).Canonical == "90h"); // never folded into days: see below
    CHECK(Dur("1", BareNumberUnit::Minutes).Canonical == "1m");
    // The old strict parser read "90" as 90 SECONDS; the editor must not.
    CHECK(Dur("90", BareNumberUnit::Minutes).Seconds != 90);
}

TEST_CASE("ParseDurationInput: zero is a valid duration, empty is not an error") {
    const DurationParse zero = Dur("0");
    CHECK(zero.Ok);
    CHECK(zero.Seconds == 0);
    CHECK(zero.Canonical == "0m");
    CHECK(Dur("0h").Ok);
    CHECK(Dur("0.0h").Canonical == "0m");

    for (const char* blank : {"", "   ", "\t", " \t "}) {
        INFO("blank: [" << std::string(blank) << "]");
        const DurationParse p = Dur(blank);
        CHECK(p.Empty);
        CHECK_FALSE(p.Ok);
        CHECK_FALSE(p.Message.IsError());
    }
}

TEST_CASE("ParseDurationInput rounds half-up to whole minutes, without floating point") {
    CHECK(Dur("0.5m").Seconds == 60); // 30 s rounds up
    CHECK(Dur("0.25m").Seconds == 0); // 15 s rounds down
    CHECK(Dur("0.4999m").Seconds == 0);
    CHECK(Dur("1.5m").Seconds == 120); // 90 s rounds up
    CHECK(Dur("1.4999m").Seconds == 60);
    CHECK(Dur("1.005h").Seconds == 3600);  // 18 s over the hour rounds down to the hour
    CHECK(Dur("1.0084h").Seconds == 3660); // 30.24 s over the hour rounds up to the next minute
    // 0.1 + 0.2 style drift cannot appear: the digits are integers.
    CHECK(Dur("0.1h 0.2h").Seconds == 1080);
    CHECK(Dur("0.3h").Seconds == 1080);
}

TEST_CASE("ParseDurationInput rejects what it cannot read and names the part that is wrong") {
    const DurationRejectCase kReject[] = {
        {"3x", ParseMessageId::DurationUnknownUnit, "x"},
        {"2h 3 parsecs", ParseMessageId::DurationUnknownUnit, "parsecs"},
        {"2d 4", ParseMessageId::DurationMissingUnit, "4"},
        {"2d 4 3h", ParseMessageId::DurationMissingUnit, "4"},
        {"abc", ParseMessageId::DurationUnexpectedText, "abc"},
        {"h", ParseMessageId::DurationUnexpectedText, "h"},
        {"2h xyz", ParseMessageId::DurationUnexpectedText, "xyz"},
        {"-1h", ParseMessageId::DurationNegative, ""},
        {"1h -30m", ParseMessageId::DurationNegative, ""},
        {"-.5h", ParseMessageId::DurationNegative, ""},
        // A dash inside a word is unreadable text, not a minus sign.
        {"not-a-duration", ParseMessageId::DurationUnexpectedText, "not-a-duration"},
        {"2h-", ParseMessageId::DurationUnexpectedText, "-"},
        {"1.5", ParseMessageId::DurationBareDecimal, "1.5"},
        {".5", ParseMessageId::DurationBareDecimal, ".5"},
        {"1:75", ParseMessageId::DurationBadClock, "1:75"},
        {"1:5", ParseMessageId::DurationBadClock, "1:5"},
        {"1:300", ParseMessageId::DurationBadClock, "1:300"},
        {"1234567890:30", ParseMessageId::DurationBadClock, "1234567890:30"},
        {"99999999999h", ParseMessageId::DurationTooLong, ""},
        {"99999999999", ParseMessageId::DurationTooLong, ""},
        {"10001w", ParseMessageId::DurationTooLong, ""},
        {"9999w 9999w", ParseMessageId::DurationTooLong, ""},
        {"1.5.5h", ParseMessageId::DurationMissingUnit, "1.5"},
    };
    for (const DurationRejectCase& c : kReject) {
        INFO("input: " << std::string(c.Input));
        const DurationParse p = Dur(c.Input);
        CHECK_FALSE(p.Ok);
        CHECK_FALSE(p.Empty);
        CHECK(p.Message.Id == c.Id);
        CHECK(p.Message.Arg == c.Arg);
        CHECK(p.Canonical.empty());
    }
}

TEST_CASE("ParseDurationInput reports where the bad part is") {
    const DurationParse unit = Dur("2h 3x");
    REQUIRE(unit.Message.Id == ParseMessageId::DurationUnknownUnit);
    CHECK(unit.Message.At == 4); // the "x"
    CHECK(unit.Message.Len == 1);

    const DurationParse missing = Dur("2d 4");
    REQUIRE(missing.Message.Id == ParseMessageId::DurationMissingUnit);
    CHECK(missing.Message.At == 3);
    CHECK(missing.Message.Len == 1);
}

TEST_CASE("ParseDurationInput survives hostile input without throwing or overflowing") {
    const std::string wall(5000, '9');
    CHECK_NOTHROW(Dur(wall.c_str()));
    CHECK(Dur(wall.c_str()).Message.Id == ParseMessageId::DurationTooLong);
    const std::string manyTokens = [] {
        std::string s;
        for (int i = 0; i < 3000; ++i) {
            s += "999w ";
        }
        return s;
    }();
    CHECK_NOTHROW(Dur(manyTokens.c_str()));
    CHECK(Dur(manyTokens.c_str()).Message.Id == ParseMessageId::DurationTooLong);
    const std::string fractionWall = "0." + std::string(4000, '7') + "h";
    CHECK(Dur(fractionWall.c_str()).Ok); // the digits past the sixth are ignored
    CHECK_NOTHROW(Dur(std::string(300, ':').c_str()));
    CHECK_NOTHROW(Dur(std::string(300, '.').c_str()));
    CHECK_NOTHROW(Dur(std::string("\xff\xfe 1h \x80").c_str()));
    CHECK_NOTHROW(Dur(std::string(1000, ' ').append("1h").c_str()));
    // A NUL inside the text ends the C string at the caller; a std::string with one is just text.
    CHECK_FALSE(ParseDurationInput(std::string("1h\0"
                                               "30m",
                                               6),
                                   BareNumberUnit::Minutes)
                    .Ok);
}

TEST_CASE("ParseDateInput: numeric and ISO dates") {
    CHECK(Iso("2026-11-20") == "2026-11-20");
    CHECK(Iso("2026/11/20") == "2026-11-20");
    CHECK(Iso("2026.11.20") == "2026-11-20");
    CHECK(Iso("11/20/2026") == "2026-11-20");
    CHECK(Iso("11/20") == "2026-11-20"); // this year
    CHECK(Iso("11/20/26") == "2026-11-20");
    CHECK(Iso("20.11.2026") == "2026-11-20"); // 20 can only be the day
    CHECK(Iso("20/11") == "2026-11-20");
    CHECK(Iso("13/1") == "2026-01-13");
    CHECK(Iso("1/13") == "2026-01-13");
    CHECK(Iso("  11/20/2026  ") == "2026-11-20");
    CHECK(Iso("2/29/2028") == "2028-02-29");
    CHECK(Iso("3000-01-01") == "3000-01-01");
    CHECK(Iso("1900-01-01") == "1900-01-01");
    // A one-digit day or month is fine; only the year group needs two or four digits.
    CHECK(Iso("2026-11-2") == "2026-11-02");
    CHECK(Iso("1/2/26") == "2026-01-02");
}

TEST_CASE("ParseDateInput: the order breaks a tie between two small numbers, and only a tie") {
    CHECK(Iso("3/4", DateOrder::MonthFirst) == "2026-03-04");
    CHECK(Iso("3/4", DateOrder::DayFirst) == "2026-04-03");
    CHECK(Iso("3/4/2027", DateOrder::MonthFirst) == "2027-03-04");
    CHECK(Iso("3/4/2027", DateOrder::DayFirst) == "2027-04-03");
    // A number over 12 fixes the order whatever the preference says.
    CHECK(Iso("20/11", DateOrder::MonthFirst) == "2026-11-20");
    CHECK(Iso("11/20", DateOrder::DayFirst) == "2026-11-20");
}

TEST_CASE("ParseDateInput: words, weekdays and offsets relative to today (Monday 2026-11-16)") {
    CHECK(WeekdayOf(Today()) == 1);
    CHECK(Iso("today") == "2026-11-16");
    CHECK(Iso("TODAY") == "2026-11-16");
    CHECK(Iso("tomorrow") == "2026-11-17");
    CHECK(Iso("tmr") == "2026-11-17");
    CHECK(Iso("yesterday") == "2026-11-15");
    CHECK(Iso("fri") == "2026-11-20");
    CHECK(Iso("friday") == "2026-11-20");
    CHECK(Iso("next fri") == "2026-11-20");
    CHECK(Iso("tue") == "2026-11-17");
    CHECK(Iso("tues") == "2026-11-17");
    CHECK(Iso("thurs") == "2026-11-19");
    CHECK(Iso("sun") == "2026-11-22");
    // The weekday of today means next week: "mon" on a Monday is a week away, never today.
    CHECK(Iso("mon") == "2026-11-23");
    CHECK(Iso("+3d") == "2026-11-19");
    CHECK(Iso("+0d") == "2026-11-16");
    CHECK(Iso("-2w") == "2026-11-02");
    CHECK(Iso("+1w") == "2026-11-23");
    CHECK(Iso("+1mo") == "2026-12-16");
    CHECK(Iso("-1mo") == "2026-10-16");
    CHECK(Iso("+2 weeks") == "2026-11-30");
    CHECK(Iso("in 3 days") == "2026-11-19");
    CHECK(Iso("in 2 weeks") == "2026-11-30");
    CHECK(Iso("3 days ago") == "2026-11-13");
    CHECK(Iso("1 week ago") == "2026-11-09");
    // Crossing a year end and a leap day.
    CHECK(Iso("+45d") == "2026-12-31");
    CHECK(Iso("+46d") == "2027-01-01");
}

TEST_CASE("ParseDateInput: a month offset clamps to the end of a shorter month") {
    const CivilDate jan31{2026, 1, 31};
    const DateParse p = ParseDateInput("+1mo", jan31, DateOrder::MonthFirst);
    REQUIRE(p.Ok);
    CHECK(FormatIsoDate(p.Date) == "2026-02-28");
    const CivilDate leap{2028, 1, 31};
    CHECK(FormatIsoDate(ParseDateInput("+1mo", leap, DateOrder::MonthFirst).Date) == "2028-02-29");
    const CivilDate dec{2026, 12, 31};
    CHECK(FormatIsoDate(ParseDateInput("+2mo", dec, DateOrder::MonthFirst).Date) == "2027-02-28");
}

TEST_CASE("ParseDateInput: month names") {
    CHECK(Iso("Nov 20") == "2026-11-20");
    CHECK(Iso("nov 20") == "2026-11-20");
    CHECK(Iso("20 nov") == "2026-11-20");
    CHECK(Iso("20 Nov 2026") == "2026-11-20");
    CHECK(Iso("November 20, 2026") == "2026-11-20");
    CHECK(Iso("nov 20th") == "2026-11-20");
    CHECK(Iso("sept 5") == "2026-09-05");
    CHECK(Iso("1st may") == "2026-05-01");
    CHECK(Iso("dec 25 27") == "2027-12-25");
    CHECK(Iso("feb 29 2028") == "2028-02-29");
}

TEST_CASE("ParseDateInput rejects what it cannot read and says why") {
    struct Reject {
        const char* Input;
        ParseMessageId Id;
        const char* Arg;
    };
    const Reject kReject[] = {
        {"banana", ParseMessageId::DateUnrecognised, "banana"},
        {"foo 20", ParseMessageId::DateUnrecognised, "foo 20"},
        {"next", ParseMessageId::DateUnrecognised, "next"},
        {"mar", ParseMessageId::DateUnrecognised, "mar"},
        {"11//20", ParseMessageId::DateUnrecognised, "11//20"},
        {"1/2/3/4", ParseMessageId::DateUnrecognised, "1/2/3/4"},
        {"123/4", ParseMessageId::DateUnrecognised, "123/4"},
        {"+3x", ParseMessageId::DateUnrecognised, "+3x"},
        {"+100000d", ParseMessageId::DateUnrecognised, "+100000d"},
        {"2/30/2026", ParseMessageId::DateNoSuchDay, "2/30/2026"},
        {"2/29/2027", ParseMessageId::DateNoSuchDay, "2/29/2027"},
        {"31 feb", ParseMessageId::DateNoSuchDay, "31 feb"},
        {"4/31", ParseMessageId::DateNoSuchDay, "4/31"},
        {"13/13", ParseMessageId::DateNoSuchDay, "13/13"},
        {"0/5", ParseMessageId::DateNoSuchDay, "0/5"},
        {"2026-13-01", ParseMessageId::DateNoSuchDay, "2026-13-01"},
        {"1/1/1800", ParseMessageId::DateYearRange, "1800"},
        {"3001-01-01", ParseMessageId::DateYearRange, "3001"},
        {"+99999d", ParseMessageId::DateYearRange, "+99999d"},
        {"+9999mo", ParseMessageId::DateYearRange, "+9999mo"},
        // A one- or three-digit year is not read, in a numeric date as in a month-name one.
        {"11/20/6", ParseMessageId::DateUnrecognised, "11/20/6"},
        {"11/20/2", ParseMessageId::DateUnrecognised, "11/20/2"},
        {"20.11.6", ParseMessageId::DateUnrecognised, "20.11.6"},
        {"11/20/202", ParseMessageId::DateUnrecognised, "11/20/202"},
        {"nov 20 6", ParseMessageId::DateUnrecognised, "nov 20 6"},
    };
    for (const Reject& c : kReject) {
        INFO("input: " << std::string(c.Input));
        const DateParse p = ParseDateInput(c.Input, Today(), DateOrder::MonthFirst);
        CHECK_FALSE(p.Ok);
        CHECK_FALSE(p.Empty);
        CHECK(p.Message.Id == c.Id);
        CHECK(p.Message.Arg == c.Arg);
    }
}

TEST_CASE("ParseDateInput: empty input and an invalid 'today' never guess") {
    const DateParse blank = ParseDateInput("   ", Today(), DateOrder::MonthFirst);
    CHECK(blank.Empty);
    CHECK_FALSE(blank.Ok);
    CHECK_FALSE(blank.Message.IsError());
    // A caller bug (today = 0000-00-00) must fail, not produce a date from garbage.
    const DateParse badToday = ParseDateInput("tomorrow", CivilDate{0, 0, 0}, DateOrder::MonthFirst);
    CHECK_FALSE(badToday.Ok);
}

TEST_CASE("AddDays and WeekdayOf agree across month, year and leap boundaries") {
    CHECK(FormatIsoDate(AddDays(CivilDate{2026, 12, 31}, 1)) == "2027-01-01");
    CHECK(FormatIsoDate(AddDays(CivilDate{2028, 2, 28}, 1)) == "2028-02-29");
    CHECK(FormatIsoDate(AddDays(CivilDate{2027, 2, 28}, 1)) == "2027-03-01");
    CHECK(FormatIsoDate(AddDays(CivilDate{2026, 3, 1}, -1)) == "2026-02-28");
    CHECK(FormatIsoDate(AddDays(CivilDate{1970, 1, 1}, 0)) == "1970-01-01");
    CHECK(FormatIsoDate(AddDays(CivilDate{2026, 11, 16}, 365)) == "2027-11-16");
    CHECK(WeekdayOf(CivilDate{1970, 1, 1}) == 4); // a Thursday
    CHECK(WeekdayOf(CivilDate{2000, 1, 1}) == 6); // a Saturday
    CHECK(WeekdayOf(CivilDate{2026, 11, 22}) == 0);
    for (int d = 0; d < 400; ++d) {
        const CivilDate a = AddDays(CivilDate{2026, 11, 16}, d);
        CHECK(WeekdayOf(a) == (1 + d) % 7);
    }
}

TEST_CASE("ParseIssueReference: keys, in any case") {
    struct Accept {
        const char* Input;
        const char* Key;
        bool FromUrl;
    };
    const Accept kAccept[] = {
        {"PROJ-12", "PROJ-12", false},
        {"proj-12", "PROJ-12", false},
        {"  PROJ-12  ", "PROJ-12", false},
        {"<PROJ-12>", "PROJ-12", false},
        {"ABC_1-5", "ABC_1-5", false},
        {"octo/repo#12", "octo/repo#12", false},
        {"https://acme.atlassian.net/browse/PROJ-12", "PROJ-12", true},
        {"acme.atlassian.net/browse/proj-12", "PROJ-12", true},
        {"https://acme.atlassian.net/browse/PROJ-12?focusedCommentId=10001", "PROJ-12", true},
        {"https://acme.atlassian.net/browse/PROJ-12#comment", "PROJ-12", true},
        {"https://acme.atlassian.net/jira/software/projects/PROJ/boards/1?selectedIssue=PROJ-12", "PROJ-12", true},
        {"https://jira.corp.example/jira/browse/OPS-7", "OPS-7", true},
        {"https://github.com/octo/repo/issues/12", "octo/repo#12", true},
        {"https://github.com/octo/repo/pull/34", "octo/repo#34", true},
        {"github.com/octo/repo/issues/12#issuecomment-1", "octo/repo#12", true},
        // A key in a query parameter is upper-cased like one in the path.
        {"https://acme.atlassian.net/jira/software/projects/PROJ/boards/1?selectedIssue=proj-7", "PROJ-7", true},
        {"https://linear.app/acme/issue/ENG-123/some-slug", "ENG-123", true},
        {"<https://acme.atlassian.net/browse/PROJ-12>", "PROJ-12", true},
        // Jira issue lists name the key after "/issues/", YouTrack after "/issue/".
        {"https://acme.atlassian.net/jira/software/projects/PROJ/issues/PROJ-12", "PROJ-12", true},
        {"https://acme.atlassian.net/issues/PROJ-12?filter=-1", "PROJ-12", true},
        {"https://jira.corp.example/projects/OPS/issues/OPS-7", "OPS-7", true},
        {"https://youtrack.corp.example/issue/OPS-7", "OPS-7", true},
        // Jira Service Management ends a queue or portal link on the key.
        {"https://acme.atlassian.net/jira/servicedesk/projects/SD/queues/custom/1/SD-123", "SD-123", true},
        {"https://help.acme.com/servicedesk/customer/portal/2/SD-123", "SD-123", true},
    };
    for (const Accept& c : kAccept) {
        INFO("input: " << std::string(c.Input));
        const IssueReferenceParse p = ParseIssueReference(c.Input);
        CHECK(p.Ok);
        CHECK(p.Key == c.Key);
        CHECK(p.FromUrl == c.FromUrl);
        CHECK_FALSE(p.Message.IsError());
    }
}

TEST_CASE("ParseIssueReference rejects text that is not an issue and says which kind of miss it is") {
    const IssueReferenceParse blank = ParseIssueReference("   ");
    CHECK(blank.Empty);
    CHECK_FALSE(blank.Ok);

    for (const char* text :
         {"hello", "12", "PROJ-", "-12", "PROJ 12", "octo/repo#0", "octo/repo#", "fix the login bug"}) {
        INFO("input: " << std::string(text));
        const IssueReferenceParse p = ParseIssueReference(text);
        CHECK_FALSE(p.Ok);
        CHECK(p.Message.Id == ParseMessageId::IssueRefNotAKey);
        CHECK(p.Key.empty());
    }
    for (const char* url :
         {"https://example.com/", "https://github.com/octo/repo/issues", "a.b/c",
          "https://acme.atlassian.net/jira/software/projects/PROJ/boards/1", "https://linear.app/acme/team/ENG/active",
          // A page or file whose name looks like a key is not an issue.
          "https://wiki.acme.com/display/ENG/Release-2", "https://github.com/o/r/blob/main/docs/ADR-12",
          "https://acme.atlassian.net/wiki/spaces/ENG/pages/123/Release-2", "docs.acme.com/specs/RFC-7",
          // A service desk queue or portal with no issue open, and a key-shaped page outside one.
          "https://acme.atlassian.net/jira/servicedesk/projects/SD/queues/custom/1",
          "https://help.acme.com/servicedesk/customer/portal/2", "https://help.acme.com/portal/2/SD-123",
          "https://acme.atlassian.net/jira/servicedesk/projects/SD/knowledge/articles/KB-12"}) {
        INFO("url: " << std::string(url));
        const IssueReferenceParse p = ParseIssueReference(url);
        CHECK_FALSE(p.Ok);
        CHECK(p.Message.Id == ParseMessageId::IssueRefNoKeyInUrl);
        CHECK(p.Key.empty());
    }
}

TEST_CASE("LooksLikeUrl and LooksLikeProjectIssueKey") {
    CHECK(LooksLikeUrl("https://x"));
    CHECK(LooksLikeUrl("HTTP://x"));
    CHECK(LooksLikeUrl("acme.atlassian.net/browse/X-1"));
    CHECK_FALSE(LooksLikeUrl("PROJ-12"));
    CHECK_FALSE(LooksLikeUrl("owner/repo#12"));
    CHECK_FALSE(LooksLikeUrl("fix a/b problem"));
    CHECK_FALSE(LooksLikeUrl("ftp:x"));
    CHECK_FALSE(LooksLikeUrl(""));
    CHECK_FALSE(LooksLikeUrl("/leading/slash"));

    CHECK(LooksLikeProjectIssueKey("A-1"));
    CHECK(LooksLikeProjectIssueKey("proj-12"));
    CHECK(LooksLikeProjectIssueKey("AB_C2-9"));
    CHECK_FALSE(LooksLikeProjectIssueKey("1A-1")); // a UUID-like or numeric prefix is not a project
    CHECK_FALSE(LooksLikeProjectIssueKey("A-"));
    CHECK_FALSE(LooksLikeProjectIssueKey("-1"));
    CHECK_FALSE(LooksLikeProjectIssueKey("A-1b"));
    CHECK_FALSE(LooksLikeProjectIssueKey(""));
}

TEST_CASE("DescribeParseMessage: every id has text, with its argument substituted and no stray specifier") {
    EnglishGuard english;
    CHECK(DescribeParseMessage(ParseMessage()).empty());

    for (int i = 1; i <= static_cast<int>(ParseMessageId::ConflictValueRejectedGeneric); ++i) {
        ParseMessage m;
        m.Id = static_cast<ParseMessageId>(i);
        m.Arg = "ARG";
        const std::string text = DescribeParseMessage(m);
        INFO("id " << i << ": " << text);
        CHECK_FALSE(text.empty());
        CHECK(text.find('%') == std::string::npos);
        CHECK(text.back() == '.');
    }
}

TEST_CASE("DescribeParseMessage: the text pins what to fix") {
    EnglishGuard english;
    auto say = [](ParseMessageId id, const char* arg) {
        ParseMessage m;
        m.Id = id;
        m.Arg = arg;
        return DescribeParseMessage(m);
    };
    CHECK(say(ParseMessageId::DurationUnknownUnit, "x") == "\"x\" is not a time unit. Use w, d, h or m, like 1h 30m.");
    CHECK(say(ParseMessageId::DurationMissingUnit, "4") == "4 needs a unit. Add w, d, h or m, like 4h.");
    CHECK(say(ParseMessageId::DurationNegative, "") == "A duration cannot be negative. Remove the minus sign.");
    CHECK(say(ParseMessageId::DurationBareDecimal, "1.5") == "1.5 has a decimal but no unit. Add h or d, like 1.5h.");
    CHECK(say(ParseMessageId::DurationBadClock, "1:75") ==
          "1:75 is not hours:minutes. Use two minute digits up to 59, like 1:30.");
    CHECK(say(ParseMessageId::DateUnrecognised, "banana") ==
          "Cannot read \"banana\" as a date. Try 2026-11-20, 11/20, tomorrow, fri or +3d.");
    CHECK(say(ParseMessageId::DateNoSuchDay, "2/30/2026") ==
          "2/30/2026 is not a day on the calendar. Check the day and the month.");
    CHECK(say(ParseMessageId::IssueRefNotAKey, "hello") ==
          "\"hello\" is not an issue key. Use a key like PROJ-123, or paste the issue link.");
    CHECK(say(ParseMessageId::QueryUnbalancedOpen, "") == "This ( has no closing ). Add ) or remove the (.");
    CHECK(say(ParseMessageId::QueryDanglingOperator, "AND") ==
          "The query ends on AND. Add what it should compare to, or remove it.");
    CHECK(say(ParseMessageId::ConflictValueRejectedEstimate, "") ==
          "That estimate cannot be used. Enter a time like 3d or 2h 30m.");
    // The tracker's own text goes in as it came; its final stop is not doubled.
    CHECK(say(ParseMessageId::QueryRejectedByTracker, "Field 'nope' does not exist.") ==
          "The tracker did not run this query: Field 'nope' does not exist.");
    CHECK(say(ParseMessageId::QueryRejectedByTracker, "Sign-in failed (100%)\r\n") ==
          "The tracker did not run this query: Sign-in failed (100%).");
    CHECK(say(ParseMessageId::QueryClausesDropped, "OR is not supported") ==
          "This tracker will ignore part of the query: OR is not supported.");
}

TEST_CASE("DescribeParseMessage follows the language, and falls back to English for a missing key") {
    ParseMessage m;
    m.Id = ParseMessageId::DurationNegative;
    SmatchetLocalization::SetLanguage("fr-FR");
    const std::string french = DescribeParseMessage(m);
    SmatchetLocalization::SetLanguage("en-US");
    const std::string english = DescribeParseMessage(m);
    CHECK(english == "A duration cannot be negative. Remove the minus sign.");
    CHECK(french != english); // the French row exists
    CHECK_FALSE(french.empty());
}

TEST_CASE("ParseDurationInput never folds hours into days or weeks (the tracker's day length is not ours)") {
    // Jira reads "d" and "w" with the instance's own working-time settings. The app does not know them, so a
    // duration typed in hours is committed in hours: "8h" must never become "1d" (7h 30m on a 7.5h-day site).
    struct Case {
        const char* Input;
        const char* Canonical;
    };
    const Case kCases[] = {
        {"8h", "8h"},
        {"12h", "12h"},
        {"24h", "24h"},
        {"40h", "40h"},
        {"7.5h", "7h 30m"},
        {"480m", "8h"},
        {"90 m", "1h 30m"},
        {"100h 30m", "100h 30m"},
        {"1:30", "1h 30m"},
        {"20:00", "20h"},
        // Days and weeks the user wrote as whole numbers are sent as written, next to hours and minutes.
        {"2d 4h", "2d 4h"},
        {"1d 12h", "1d 12h"},
        {"1w", "1w"},
        {"1w 2d 3h 4m", "1w 2d 3h 4m"},
        {"2d", "2d"},
        {"2d 90m", "2d 1h 30m"},
        {"3d 8h", "3d 8h"},
        // A decimal day has no whole-number spelling, so it falls to hours (the app's 8-hour day, documented).
        {".5d", "4h"},
        {"1.5d", "12h"},
    };
    for (const Case& c : kCases) {
        INFO("input: " << std::string(c.Input));
        const DurationParse p = Dur(c.Input);
        CHECK(p.Ok);
        CHECK(p.Canonical == c.Canonical);
    }
}
