#pragma once

// Tolerant parsing of what a user types into a field, with a fix-it message when it does not parse
// (INPUT_UX_AUDIT.md § 4.5). Every parser here returns either a value plus the canonical text the rest
// of the app already understands, or a ParseMessage that names the part that is wrong. Editors commit
// the canonical text, so the strict parsers downstream (`ParseWorkDurationToSeconds`, `TryParseJiraDateTime`,
// the payload builders) see exactly what they saw before.
//
// Pure: no ImGui, no I/O, no localization. `DescribeParseMessage` (TrackerFieldValueParseExplainText.cpp) is
// the one function that turns a message into user text; it is the only part that calls SmatchetLocalization.

#include <cstddef>
#include <string>
#include <vector>

namespace TrackerParseExplain {

/// Closed set of reasons a parse failed. Each id has exactly one user text in DescribeParseMessage.
enum class ParseMessageId : unsigned char {
    None, ///< no problem (a success, or nothing typed)

    // Durations
    DurationUnknownUnit,      ///< Arg: the unit text, e.g. "x" in "3x"
    DurationMissingUnit,      ///< Arg: the number with no unit, e.g. "4" in "2d 4"
    DurationUnexpectedText,   ///< Arg: the text that is not part of a duration
    DurationNegative,         ///< a minus sign
    DurationBareDecimal,      ///< Arg: the number, e.g. "1.5" (minutes or hours?)
    DurationBadClock,         ///< Arg: the h:mm text, e.g. "1:75"
    DurationTooLong,          ///< over the 10000-week ceiling
    DurationRequiredPositive, ///< caller policy: zero is not enough here (time spent)

    // Dates
    DateUnrecognised, ///< Arg: the text
    DateNoSuchDay,    ///< Arg: the date as typed, e.g. "2/30/2026"
    DateYearRange,    ///< Arg: the year

    // Issue references
    IssueRefNoKeyInUrl, ///< a URL with no issue key in it
    IssueRefNotAKey,    ///< Arg: the text

    // Site URLs and paths (TrackerInputNormalisePure)
    SiteUrlHasSpaces,         ///< a URL or host containing whitespace
    SiteUrlNotHttp,           ///< Arg: the scheme, e.g. "ftp"
    SiteUrlNoHost,            ///< nothing that looks like a host
    OwnerRepoShape,           ///< Arg: the text; expected owner or owner/repo
    PathIsFolder,             ///< Arg: the path; ends with a separator
    PathHasLineBreak,         ///< a newline inside a path
    HttpUrlMissingScheme,     ///< Arg: the text
    HttpUrlUnsupportedScheme, ///< Arg: the scheme

    // Queries (JqlQueryValidatePure)
    QueryUnbalancedOpen,   ///< a "(" with no ")"
    QueryUnbalancedClose,  ///< Arg: nothing; a ")" with no "("
    QueryUnclosedQuote,    ///< a quote that is never closed
    QueryDanglingOperator, ///< Arg: the operator or keyword at the end, e.g. "AND", "="
    QueryUnknownField,     ///< Arg: the field name

    // Conflict resolver (field-specific hints for a value the replay refused)
    ConflictValueRejectedSprint,   ///< the sprint name or id was not found
    ConflictValueRejectedEstimate, ///< the estimate could not be used
    ConflictValueRejectedGeneric,  ///< any other field
};

/// Why a parse failed, and where. `At`/`Len` are a byte span into the trimmed input when the failure has
/// one place (Len 0 when it is about the whole text).
struct ParseMessage {
    ParseMessageId Id = ParseMessageId::None;
    std::string Arg;
    std::size_t At = 0;
    std::size_t Len = 0;

    bool IsError() const { return Id != ParseMessageId::None; }
};

/// A ParseMessage. The one builder every parser here uses.
ParseMessage MakeParseMessage(ParseMessageId id, const std::string& arg, std::size_t at, std::size_t len);

/// The parts of a pasted address that the readers here need. Anything may be missing: "acme.atlassian.net/browse/X-1"
/// has no scheme, and "https://host" has no segments. Lenient on purpose: it never rejects, callers judge the parts.
struct UrlParts {
    std::string Scheme;                ///< lower-case, "" when none was typed
    std::string Host;                  ///< lower-case, with any ":port", no trailing dots
    std::vector<std::string> Segments; ///< the path split on "/", empty segments dropped
    std::string Query;                 ///< after "?", before "#"
};

/// Splits `url` (with or without a scheme) into its parts; the fragment is dropped.
UrlParts SplitUrl(const std::string& url);

/// User text for `msg`, localized, with its argument substituted. Empty for ParseMessageId::None.
std::string DescribeParseMessage(const ParseMessage& msg);

// Durations

/// What a number with no unit means. Jira's default unit is minutes.
enum class BareNumberUnit : unsigned char { Minutes, Hours };

struct DurationParse {
    bool Ok = false;       ///< true: Seconds and Canonical are set
    bool Empty = false;    ///< nothing typed (Ok is false; not an error by itself)
    long long Seconds = 0; ///< whole minutes, in seconds; 0 is a valid duration ("0m")
    std::string Canonical; ///< "1h 30m", "0m" — the text to commit
    ParseMessage Message;  ///< why it failed, when !Ok && !Empty
};

/// Parse a typed duration. Accepts, case-insensitively and with optional spaces between a number and
/// its unit:
/// - unit tokens w/d/h/m and their words (wk, week(s), day(s), hr(s), hour(s), min(s), minute(s)),
///   in any order, summed: "2d 4h", "3h30m", "90 m", "1 hour 15 mins"
/// - decimals on a unit: "1.5h", ".5d", "0,5h" (a comma between digits is a decimal point)
/// - "1h30" (a trailing number after hours is minutes) and "1:30" (h:mm)
/// - a bare whole number, read as `bareUnit`: "90" with Minutes is 1h 30m
/// A week is 5 days and a day is 8 hours (Jira's defaults, as FormatWorkDurationFromSeconds uses).
/// The total rounds half-up to whole minutes. Never throws.
DurationParse ParseDurationInput(const std::string& input, BareNumberUnit bareUnit);

// Dates

/// How a numeric date with two small numbers ("3/4") is read. A number over 12 fixes the order.
enum class DateOrder : unsigned char { MonthFirst, DayFirst };

struct CivilDate {
    int Year = 0;
    int Month = 0; ///< 1-12
    int Day = 0;   ///< 1-31
};

struct DateParse {
    bool Ok = false;
    bool Empty = false;
    CivilDate Date;
    ParseMessage Message;
};

/// Day of the week of `d`, 0 = Sunday .. 6 = Saturday. `d` must be a valid date.
int WeekdayOf(const CivilDate& d);

/// `d` moved by `days` (negative goes back). `d` must be a valid date.
CivilDate AddDays(const CivilDate& d, int days);

/// Parse a typed date relative to `today` (the user's local date). Accepts, case-insensitively:
/// - ISO "2026-11-20" and "2026/11/20"
/// - numeric "11/20/2026", "20.11.2026", "11-20-26", "11/20" (this year); `order` breaks a tie
/// - month names: "Nov 20", "20 nov", "November 20, 2026", "20 Nov 2026"
/// - "today", "tomorrow" ("tmr"), "yesterday"
/// - a weekday ("fri", "friday", "next fri"): the next one after today, 1 to 7 days ahead
/// - offsets "+3d", "-2w", "+1mo", "in 3 days", "in 2 weeks", "3 days ago"
/// Years are 1900-3000. A two-digit year is 20yy, a one- or three-digit year is not read. Never throws.
DateParse ParseDateInput(const std::string& input, const CivilDate& today, DateOrder order);

/// "2026-11-20". `d` must be a valid date.
std::string FormatIsoDate(const CivilDate& d);

// Issue references

/// What a user typed or pasted to name an issue: a key, or a URL with one in it. The key comes out in the
/// shape the owning backend already uses (Jira/Linear "PROJ-12", GitHub "owner/repo#12"); this does not
/// interpret it further (CONTEXT.md § Issue reference).
struct IssueReferenceParse {
    bool Ok = false;
    bool Empty = false;
    bool FromUrl = false; ///< the key came out of a URL
    std::string Key;
    ParseMessage Message;
};

/// True when `text` (already trimmed) starts like a URL: a scheme ("https://") or a host with a dot
/// followed by a "/" ("acme.atlassian.net/browse/X-1").
bool LooksLikeUrl(const std::string& text);

/// True for a Jira-style key "PROJ-12" (letter first; letters, digits, '_' before the dash; digits after).
/// Case-insensitive; IssueReferenceParse::Key is upper-cased.
bool LooksLikeProjectIssueKey(const std::string& text);

/// Parse a typed or pasted issue reference. Accepts:
/// - a key, any case: "proj-12" -> "PROJ-12"; a GitHub key "owner/repo#12" as it is
/// - a Jira URL: ".../browse/PROJ-12", ".../issues/PROJ-12", "...?selectedIssue=PROJ-12"
/// - a GitHub URL: "github.com/owner/repo/issues/12" or ".../pull/12" -> "owner/repo#12"
/// - a Linear URL: "linear.app/<workspace>/issue/ENG-123/<slug>" -> "ENG-123"
/// - a Jira Service Management queue or portal link, read by the key at the end of its path:
///   ".../servicedesk/projects/SD/queues/custom/1/SD-12", ".../servicedesk/customer/portal/2/SD-12"
/// - another tracker's URL where the key directly follows "/issue/" (YouTrack)
/// A key-shaped segment anywhere else in a path is not read as an issue.
IssueReferenceParse ParseIssueReference(const std::string& input);

} // namespace TrackerParseExplain
