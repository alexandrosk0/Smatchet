#include "SmatchetLocalization.h"
#include "TrackerFieldValueParseExplain.h"

#include <string>

namespace TrackerParseExplain {

// The only place the parse layer turns a message into words. Every case is one literal
// SmatchetLocalization call, so each string has its call-site English and a French row in kEntries[]
// (INPUT_UX_AUDIT.md § 4.5). Each text names the part that is wrong and says how to fix it.
std::string DescribeParseMessage(const ParseMessage& msg) {
    using SmatchetLocalization::Format;
    using SmatchetLocalization::T;
    const char* const arg = msg.Arg.c_str();
    switch (msg.Id) {
    case ParseMessageId::None:
        return std::string();

    case ParseMessageId::DurationUnknownUnit:
        return Format("parse.duration.unknown_unit", "\"%s\" is not a time unit. Use w, d, h or m, like 1h 30m.", arg);
    case ParseMessageId::DurationMissingUnit:
        return Format("parse.duration.missing_unit", "%s needs a unit. Add w, d, h or m, like %sh.", arg, arg);
    case ParseMessageId::DurationUnexpectedText:
        return Format("parse.duration.unexpected_text",
                      "\"%s\" is not part of a duration. Use numbers with w, d, h or m, like 2d 4h.", arg);
    case ParseMessageId::DurationNegative:
        return T("parse.duration.negative", "A duration cannot be negative. Remove the minus sign.");
    case ParseMessageId::DurationBareDecimal:
        return Format("parse.duration.bare_decimal", "%s has a decimal but no unit. Add h or d, like %sh.", arg, arg);
    case ParseMessageId::DurationBadClock:
        return Format("parse.duration.bad_clock", "%s is not hours:minutes. Use two minute digits up to 59, like 1:30.",
                      arg);
    case ParseMessageId::DurationTooLong:
        return T("parse.duration.too_long", "That duration is too long. Use 10000 weeks or less.");
    case ParseMessageId::DurationRequiredPositive:
        return T("parse.duration.required_positive", "Enter a time above zero, like 30m or 1h 30m.");

    case ParseMessageId::DateUnrecognised:
        return Format("parse.date.unrecognised",
                      "Cannot read \"%s\" as a date. Try 2026-11-20, 11/20, tomorrow, fri or +3d.", arg);
    case ParseMessageId::DateNoSuchDay:
        return Format("parse.date.no_such_day", "%s is not a day on the calendar. Check the day and the month.", arg);
    case ParseMessageId::DateYearRange:
        return Format("parse.date.year_range", "%s is outside the years 1900 to 3000. Check the year.", arg);

    case ParseMessageId::IssueRefNoKeyInUrl:
        return T("parse.issue.no_key_in_url",
                 "That link has no issue key in it. Paste the issue's own link, or its key.");
    case ParseMessageId::IssueRefNotAKey:
        return Format("parse.issue.not_a_key",
                      "\"%s\" is not an issue key. Use a key like PROJ-123, or paste the issue link.", arg);

    case ParseMessageId::SiteUrlHasSpaces:
        return T("parse.site.has_spaces", "This has a space in it. Remove the spaces.");
    case ParseMessageId::SiteUrlNotHttp:
        return Format("parse.site.not_http", "\"%s://\" is not supported. Use https, or just the host name.", arg);
    case ParseMessageId::SiteUrlNoHost:
        return T("parse.site.no_host", "Cannot find a host name here. Use a name like acme.atlassian.net.");
    case ParseMessageId::OwnerRepoShape:
        return Format("parse.site.owner_repo", "\"%s\" is not an owner or owner/repo. Use letters, digits, - and _.",
                      arg);
    case ParseMessageId::PathIsFolder:
        return T("parse.path.is_folder", "This is a folder. Add the program's file name at the end.");
    case ParseMessageId::PathHasLineBreak:
        return T("parse.path.line_break", "This has a line break in it. Paste the path on one line.");
    case ParseMessageId::HttpUrlMissingScheme:
        return Format("parse.url.missing_scheme", "\"%s\" needs https:// at the start.", arg);
    case ParseMessageId::HttpUrlUnsupportedScheme:
        return Format("parse.url.unsupported_scheme", "\"%s://\" cannot be opened. Use an https:// or http:// link.",
                      arg);

    case ParseMessageId::QueryUnbalancedOpen:
        return T("parse.query.open_paren", "This ( has no closing ). Add ) or remove the (.");
    case ParseMessageId::QueryUnbalancedClose:
        return T("parse.query.close_paren", "This ) has no opening (. Add ( or remove the ).");
    case ParseMessageId::QueryUnclosedQuote:
        return Format("parse.query.open_quote", "This %s quote is never closed. Add the closing %s.", arg, arg);
    case ParseMessageId::QueryDanglingOperator:
        return Format("parse.query.dangling", "The query ends on %s. Add what it should compare to, or remove it.",
                      arg);
    case ParseMessageId::QueryUnknownField:
        return Format("parse.query.unknown_field",
                      "\"%s\" is not a field this tracker lists. Check the spelling, or pick one from the suggestions.",
                      arg);

    case ParseMessageId::ConflictValueRejectedSprint:
        return T("parse.conflict.sprint", "That sprint was not found. Enter a sprint's name or its id.");
    case ParseMessageId::ConflictValueRejectedEstimate:
        return T("parse.conflict.estimate", "That estimate cannot be used. Enter a time like 3d or 2h 30m.");
    case ParseMessageId::ConflictValueRejectedGeneric:
        return T("parse.conflict.generic",
                 "That value cannot be used for this field. Enter one of the field's own values.");
    }
    return std::string();
}

} // namespace TrackerParseExplain
