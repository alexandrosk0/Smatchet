#include "TrackerFieldValueParseExplain.h"

#include "StringUtil.h"
#include "TrackerDateTimePure.h"
#include "TrackerFieldValueParser.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace TrackerParseExplain {

namespace {

// Jira's time-tracking defaults: a working day is 8 hours, a working week 5 days.
constexpr long long kSecondsPerMinute = 60;
constexpr long long kSecondsPerHour = 60 * kSecondsPerMinute;
constexpr long long kSecondsPerDay = 8 * kSecondsPerHour;
constexpr long long kSecondsPerWeek = 5 * kSecondsPerDay;
/// Anything longer is a typo. 10000 weeks keeps every product well inside long long.
constexpr long long kMaxDurationSeconds = 10000LL * kSecondsPerWeek;
/// Digit-run caps applied before any arithmetic, so a pasted wall of digits can never overflow.
constexpr std::size_t kMaxIntegerDigits = 9;
constexpr std::size_t kMaxFractionDigits = 6;

constexpr int kMinYear = 1900;
constexpr int kMaxYear = 3000;
/// Relative offsets beyond a century are typos.
constexpr int kMaxOffsetDays = 36600;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsSpace(char c) { return c == ' ' || c == '\t'; }
char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

void SkipSpaces(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && IsSpace(s[pos])) {
        ++pos;
    }
}

/// Reads a run of ASCII letters at `pos`, lower-cased.
std::string ReadWord(const std::string& s, std::size_t& pos) {
    std::string word;
    while (pos < s.size() && IsAsciiAlpha(s[pos])) {
        word.push_back(LowerAscii(s[pos]));
        ++pos;
    }
    return word;
}

/// The text from `pos` to the next space, for an "unexpected text" message.
std::string RunUntilSpace(const std::string& s, std::size_t pos) {
    std::size_t end = pos;
    while (end < s.size() && !IsSpace(s[end])) {
        ++end;
    }
    return s.substr(pos, end - pos);
}

/// A non-negative decimal number as integer part + fraction digits, so scaling never uses floating point.
struct Decimal {
    long long Whole = 0;
    long long Fraction = 0;      ///< the digits after the point, as an integer
    long long FractionScale = 1; ///< 10^(number of fraction digits)
    bool HasPoint = false;
    bool TooManyDigits = false;
    std::size_t Begin = 0;
    std::size_t End = 0;
};

/// Reads "12", "1.5", ".5" or "0,5" at `pos`. A comma is a decimal point only between two digits, so
/// "1h, 30m" still separates tokens. Returns false (pos unchanged) when no number starts here.
bool ReadDecimal(const std::string& s, std::size_t& pos, Decimal& out) {
    std::size_t p = pos;
    const bool leadingPoint = p + 1 < s.size() && s[p] == '.' && IsDigit(s[p + 1]);
    if (!leadingPoint && (p >= s.size() || !IsDigit(s[p]))) {
        return false;
    }
    Decimal d;
    d.Begin = p;
    std::size_t wholeDigits = 0;
    while (p < s.size() && IsDigit(s[p])) {
        if (++wholeDigits > kMaxIntegerDigits) {
            d.TooManyDigits = true;
        } else {
            d.Whole = d.Whole * 10 + (s[p] - '0');
        }
        ++p;
    }
    const bool pointHere = p + 1 < s.size() && (s[p] == '.' || (s[p] == ',' && wholeDigits > 0)) && IsDigit(s[p + 1]);
    if (pointHere) {
        d.HasPoint = true;
        ++p;
        std::size_t fractionDigits = 0;
        while (p < s.size() && IsDigit(s[p])) {
            if (fractionDigits < kMaxFractionDigits) {
                d.Fraction = d.Fraction * 10 + (s[p] - '0');
                d.FractionScale *= 10;
                ++fractionDigits;
            }
            ++p;
        }
    }
    d.End = p;
    out = d;
    pos = p;
    return true;
}

/// Seconds as a whole part plus a fraction in millionths, so a sum of tokens is rounded once, at the end.
struct ExactSeconds {
    long long Whole = 0;
    long long Micro = 0; ///< 0 .. 999999
};

constexpr long long kMicroPerSecond = 1000000;

/// `value * unitSeconds` without rounding. The caller has capped the digit runs (Whole < 1e9, at most six
/// fraction digits), so every product here stays far below LLONG_MAX and FractionScale divides 1e6 exactly.
ExactSeconds ScaleDecimal(const Decimal& value, long long unitSeconds) {
    ExactSeconds out;
    const long long fractionTimesUnit = value.Fraction * unitSeconds;
    out.Whole = value.Whole * unitSeconds + fractionTimesUnit / value.FractionScale;
    out.Micro = (fractionTimesUnit % value.FractionScale) * (kMicroPerSecond / value.FractionScale);
    return out;
}

void AddExact(ExactSeconds& total, const ExactSeconds& part) {
    total.Whole += part.Whole;
    total.Micro += part.Micro;
    if (total.Micro >= kMicroPerSecond) {
        total.Micro -= kMicroPerSecond;
        ++total.Whole;
    }
}

/// Seconds per unit word, or 0 when the word is not a duration unit.
long long UnitSeconds(const std::string& word) {
    static const char* const kWeeks[] = {"w", "wk", "wks", "week", "weeks"};
    static const char* const kDays[] = {"d", "day", "days"};
    static const char* const kHours[] = {"h", "hr", "hrs", "hour", "hours"};
    static const char* const kMinutes[] = {"m", "min", "mins", "minute", "minutes"};
    for (const char* w : kWeeks) {
        if (word == w) {
            return kSecondsPerWeek;
        }
    }
    for (const char* w : kDays) {
        if (word == w) {
            return kSecondsPerDay;
        }
    }
    for (const char* w : kHours) {
        if (word == w) {
            return kSecondsPerHour;
        }
    }
    for (const char* w : kMinutes) {
        if (word == w) {
            return kSecondsPerMinute;
        }
    }
    return 0;
}

DurationParse DurationFailure(ParseMessageId id, const std::string& arg, std::size_t at, std::size_t len) {
    DurationParse r;
    r.Message = MakeParseMessage(id, arg, at, len);
    return r;
}

/// The whole weeks and days the user wrote as plain numbers ("2d", "1w"). They are sent back as typed.
struct WrittenDaysWeeks {
    long long Weeks = 0;
    long long Days = 0;
};

/// The canonical text for a whole number of minutes: "1h 30m", "45m", "0m", "2d 4h".
///
/// Hours are never folded into days or weeks. A day is 8 hours here only because this app's own duration
/// display assumes it; the tracker reads "d" and "w" with the instance's own working-time settings (7.5h days,
/// 24h days), which the app does not know, so "12h" must stay "12h" and never become "1d 4h". Days and weeks
/// the user wrote as whole numbers are kept as written; everything else (hours, minutes, a decimal day,
/// "1:30", a bare number) is written as hours and minutes. 60 minutes is always one hour.
std::string CanonicalDuration(long long seconds, const WrittenDaysWeeks& written) {
    if (seconds <= 0) {
        return "0m";
    }
    long long rest = seconds - written.Weeks * kSecondsPerWeek - written.Days * kSecondsPerDay;
    if (rest < 0) {
        rest = 0; // cannot happen: the written days and weeks are part of the total
    }
    std::string text;
    const auto append = [&text](long long count, char unit) {
        if (count > 0) {
            if (!text.empty()) {
                text.push_back(' ');
            }
            text += std::to_string(count);
            text.push_back(unit);
        }
    };
    append(written.Weeks, 'w');
    append(written.Days, 'd');
    append(rest / kSecondsPerHour, 'h');
    append((rest % kSecondsPerHour) / kSecondsPerMinute, 'm');
    return text.empty() ? std::string("0m") : text;
}

/// Rounds half-up to whole minutes, once: the fraction below a second never tips a minute (29.994 s is 0 min).
DurationParse DurationSuccess(const ExactSeconds& exact, const WrittenDaysWeeks& written = WrittenDaysWeeks()) {
    DurationParse r;
    r.Ok = true;
    r.Seconds = ((exact.Whole + kSecondsPerMinute / 2) / kSecondsPerMinute) * kSecondsPerMinute;
    r.Canonical = CanonicalDuration(r.Seconds, written);
    return r;
}

ExactSeconds Whole(long long seconds) {
    ExactSeconds e;
    e.Whole = seconds;
    return e;
}

/// "1:30" (h:mm). Returns true when `s` has that shape, with the result (or the error) in `out`.
bool TryParseClockDuration(const std::string& s, DurationParse& out) {
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i != colon && !IsDigit(s[i])) {
            return false;
        }
    }
    const std::string hoursText = s.substr(0, colon);
    const std::string minutesText = s.substr(colon + 1);
    if (hoursText.size() > kMaxIntegerDigits || minutesText.size() != 2) {
        out = DurationFailure(ParseMessageId::DurationBadClock, s, 0, s.size());
        return true;
    }
    const long long hours = std::strtoll(hoursText.c_str(), nullptr, 10);
    const long long minutes = std::strtoll(minutesText.c_str(), nullptr, 10);
    if (minutes > 59) {
        out = DurationFailure(ParseMessageId::DurationBadClock, s, 0, s.size());
        return true;
    }
    const long long total = hours * kSecondsPerHour + minutes * kSecondsPerMinute;
    out = total > kMaxDurationSeconds ? DurationFailure(ParseMessageId::DurationTooLong, std::string(), 0, 0)
                                      : DurationSuccess(Whole(total));
    return true;
}

/// The whole text is one number with no unit. Returns true when that is the shape.
bool TryParseBareNumber(const std::string& s, BareNumberUnit bareUnit, DurationParse& out) {
    std::size_t pos = 0;
    Decimal value;
    if (!ReadDecimal(s, pos, value) || pos != s.size()) {
        return false;
    }
    if (value.TooManyDigits) {
        out = DurationFailure(ParseMessageId::DurationTooLong, std::string(), 0, 0);
    } else if (value.HasPoint) {
        out = DurationFailure(ParseMessageId::DurationBareDecimal, s, 0, s.size());
    } else {
        const long long unit = bareUnit == BareNumberUnit::Hours ? kSecondsPerHour : kSecondsPerMinute;
        const long long total = value.Whole * unit;
        out = total > kMaxDurationSeconds ? DurationFailure(ParseMessageId::DurationTooLong, std::string(), 0, 0)
                                          : DurationSuccess(Whole(total));
    }
    return true;
}

/// Skips the separators allowed between duration tokens: spaces, commas, '+' and the word "and".
void SkipDurationSeparators(const std::string& s, std::size_t& pos) {
    for (;;) {
        const std::size_t before = pos;
        while (pos < s.size() && (IsSpace(s[pos]) || s[pos] == ',' || s[pos] == '+')) {
            ++pos;
        }
        if (pos + 3 <= s.size() && LowerAscii(s[pos]) == 'a' && LowerAscii(s[pos + 1]) == 'n' &&
            LowerAscii(s[pos + 2]) == 'd' && (pos + 3 == s.size() || !IsAsciiAlpha(s[pos + 3]))) {
            pos += 3;
        }
        if (pos == before) {
            return;
        }
    }
}

/// The unit-token scan: "2d 4h", "1.5h", "90 m", "1h30".
DurationParse ParseDurationTokens(const std::string& s) {
    ExactSeconds total;
    WrittenDaysWeeks written;
    long long lastUnit = 0;
    std::size_t pos = 0;
    SkipDurationSeparators(s, pos);
    while (pos < s.size()) {
        Decimal value;
        if (!ReadDecimal(s, pos, value)) {
            const std::string bad = RunUntilSpace(s, pos);
            return DurationFailure(ParseMessageId::DurationUnexpectedText, bad, pos, bad.size());
        }
        if (value.TooManyDigits) {
            return DurationFailure(ParseMessageId::DurationTooLong, std::string(), 0, 0);
        }
        const std::string numberText = s.substr(value.Begin, value.End - value.Begin);
        SkipSpaces(s, pos);
        const std::size_t unitAt = pos;
        const std::string word = ReadWord(s, pos);
        long long unit = 0;
        if (word.empty()) {
            // "1h30": a last number straight after hours is minutes. Anywhere else a unit is missing.
            SkipDurationSeparators(s, pos);
            if (pos < s.size() || lastUnit != kSecondsPerHour) {
                return DurationFailure(ParseMessageId::DurationMissingUnit, numberText, value.Begin, numberText.size());
            }
            unit = kSecondsPerMinute;
        } else {
            unit = UnitSeconds(word);
            if (unit == 0) {
                return DurationFailure(ParseMessageId::DurationUnknownUnit, word, unitAt, word.size());
            }
        }
        AddExact(total, ScaleDecimal(value, unit));
        if (!value.HasPoint && unit == kSecondsPerWeek) {
            written.Weeks += value.Whole;
        } else if (!value.HasPoint && unit == kSecondsPerDay) {
            written.Days += value.Whole;
        }
        if (total.Whole > kMaxDurationSeconds) {
            return DurationFailure(ParseMessageId::DurationTooLong, std::string(), 0, 0);
        }
        lastUnit = unit;
        SkipDurationSeparators(s, pos);
    }
    return DurationSuccess(total, written);
}

/// Where a minus sign starts a number ("-1h", "1h -30m"), or npos. A dash inside a word ("not-a-duration") is
/// not a sign, so it is reported as the unreadable text it is.
std::size_t FindNegativeSign(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '-' || i + 1 >= s.size()) {
            continue;
        }
        const bool numberFollows = IsDigit(s[i + 1]) || (s[i + 1] == '.' && i + 2 < s.size() && IsDigit(s[i + 2]));
        if (numberFollows) {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace

ParseMessage MakeParseMessage(ParseMessageId id, const std::string& arg, std::size_t at, std::size_t len) {
    ParseMessage m;
    m.Id = id;
    m.Arg = arg;
    m.At = at;
    m.Len = len;
    return m;
}

UrlParts SplitUrl(const std::string& url) {
    UrlParts out;
    std::string rest = url;
    const std::size_t fragment = rest.find('#');
    if (fragment != std::string::npos) {
        rest.resize(fragment);
    }
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) {
        out.Scheme = ToLowerAsciiCopy(rest.substr(0, scheme));
        rest = rest.substr(scheme + 3);
    }
    const std::size_t query = rest.find('?');
    if (query != std::string::npos) {
        out.Query = rest.substr(query + 1);
        rest.resize(query);
    }
    const std::size_t hostEnd = rest.find('/');
    out.Host = ToLowerAsciiCopy(rest.substr(0, hostEnd));
    while (!out.Host.empty() && out.Host.back() == '.') {
        out.Host.pop_back();
    }
    // Path segments: split on "/", drop the empty ones ("/a//b/" -> a, b).
    for (std::size_t at = hostEnd; at != std::string::npos && at < rest.size();) {
        const std::size_t next = rest.find('/', at + 1);
        const std::string segment = rest.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
        if (!segment.empty()) {
            out.Segments.push_back(segment);
        }
        at = next;
    }
    return out;
}

DurationParse ParseDurationInput(const std::string& input, BareNumberUnit bareUnit) {
    const std::string s = TrimCopyAsciiWhitespace(input);
    if (s.empty()) {
        DurationParse r;
        r.Empty = true;
        return r;
    }
    const std::size_t minus = FindNegativeSign(s);
    if (minus != std::string::npos) {
        return DurationFailure(ParseMessageId::DurationNegative, std::string(), minus, 1);
    }
    DurationParse r;
    if (TryParseClockDuration(s, r) || TryParseBareNumber(s, bareUnit, r)) {
        return r;
    }
    return ParseDurationTokens(s);
}

// Dates

namespace {

/// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil).
long long DaysFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

CivilDate CivilFromDays(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    CivilDate out;
    out.Day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    out.Month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    out.Year = static_cast<int>(yoe + era * 400 + (out.Month <= 2 ? 1 : 0));
    return out;
}

bool IsValidDate(int y, int m, int d) {
    return m >= 1 && m <= 12 && d >= 1 && d <= TrackerDateTimePure::DaysInMonth(y, m);
}

DateParse DateFailure(ParseMessageId id, const std::string& arg) {
    DateParse r;
    r.Message = MakeParseMessage(id, arg, 0, arg.size());
    return r;
}

DateParse DateSuccess(const CivilDate& d) {
    DateParse r;
    r.Ok = true;
    r.Date = d;
    return r;
}

/// Checks a year/month/day triple; `typed` is quoted in the message when the day does not exist.
DateParse CheckedDate(int y, int m, int d, const std::string& typed) {
    if (y < kMinYear || y > kMaxYear) {
        return DateFailure(ParseMessageId::DateYearRange, std::to_string(y));
    }
    if (!IsValidDate(y, m, d)) {
        return DateFailure(ParseMessageId::DateNoSuchDay, typed);
    }
    CivilDate out;
    out.Year = y;
    out.Month = m;
    out.Day = d;
    return DateSuccess(out);
}

/// Two-digit years are 20yy; anything else is taken as written.
int ExpandYear(int year, std::size_t digits) { return digits <= 2 ? 2000 + year : year; }

/// Splits "11/20/2026", "2026-11-20" or "20.11" into digit groups. False when the text is anything else
/// (letters, a mix of separators is fine, two separators in a row is not).
bool SplitNumericDate(const std::string& s, std::vector<std::string>& groups) {
    groups.clear();
    std::string current;
    for (char c : s) {
        if (IsDigit(c)) {
            current.push_back(c);
            continue;
        }
        if ((c != '/' && c != '.' && c != '-') || current.empty()) {
            return false;
        }
        groups.push_back(current);
        current.clear();
    }
    if (current.empty()) {
        return false;
    }
    groups.push_back(current);
    for (const std::string& g : groups) {
        if (g.size() > 4) {
            return false;
        }
    }
    return groups.size() == 2 || groups.size() == 3;
}

/// Orders two day/month numbers. A number over 12 can only be the day; otherwise `order` decides.
void ResolveDayMonth(int a, int b, DateOrder order, int& month, int& day) {
    const bool dayFirst = a > 12 || (b <= 12 && order == DateOrder::DayFirst);
    month = dayFirst ? b : a;
    day = dayFirst ? a : b;
}

bool TryParseNumericDate(const std::string& s, const CivilDate& today, DateOrder order, DateParse& out) {
    std::vector<std::string> groups;
    if (!SplitNumericDate(s, groups)) {
        return false;
    }
    const int a = std::atoi(groups[0].c_str());
    const int b = std::atoi(groups[1].c_str());
    if (groups.size() == 3 && groups[0].size() == 4) {
        out = CheckedDate(a, b, std::atoi(groups[2].c_str()), s); // ISO order
        return true;
    }
    if (groups[0].size() > 2 || groups[1].size() > 2 || (groups.size() == 3 && groups[2].size() == 3)) {
        out = DateFailure(ParseMessageId::DateUnrecognised, s);
        return true;
    }
    const int year = groups.size() == 3 ? ExpandYear(std::atoi(groups[2].c_str()), groups[2].size()) : today.Year;
    int month = 0;
    int day = 0;
    ResolveDayMonth(a, b, order, month, day);
    out = CheckedDate(year, month, day, s);
    return true;
}

/// 1-12 for a month name or an abbreviation of at least three letters ("nov", "sept", "november").
int MonthFromWord(const std::string& word) {
    static const char* const kMonths[] = {"january", "february", "march",     "april",   "may",      "june",
                                          "july",    "august",   "september", "october", "november", "december"};
    if (word.size() < 3) {
        return 0;
    }
    for (int i = 0; i < 12; ++i) {
        const std::string full = kMonths[i];
        if (word.size() <= full.size() && full.compare(0, word.size(), word) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/// 0-6 (Sunday first) for a weekday name or abbreviation ("fri", "tues", "thurs"), else -1.
int WeekdayFromWord(const std::string& word) {
    static const char* const kDays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    if (word.size() < 3) {
        return -1;
    }
    for (int i = 0; i < 7; ++i) {
        const std::string full = kDays[i];
        if (word.size() <= full.size() && full.compare(0, word.size(), word) == 0) {
            return i;
        }
    }
    return -1;
}

/// Splits on spaces and commas into lower-case words and numbers; "20th" becomes "20".
std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> words;
    std::string current;
    for (char c : s) {
        if (IsSpace(c) || c == ',') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(LowerAscii(c));
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    for (std::string& w : words) {
        static const char* const kSuffixes[] = {"st", "nd", "rd", "th"};
        for (const char* suffix : kSuffixes) {
            if (w.size() > 2 && IsDigit(w[w.size() - 3]) && w.compare(w.size() - 2, 2, suffix) == 0) {
                w.resize(w.size() - 2);
            }
        }
    }
    return words;
}

bool AllDigits(const std::string& w) {
    if (w.empty() || w.size() > 4) {
        return false;
    }
    for (char c : w) {
        if (!IsDigit(c)) {
            return false;
        }
    }
    return true;
}

/// "Nov 20", "20 nov", "November 20 2026", "20 Nov 2026".
bool TryParseMonthNameDate(const std::vector<std::string>& w, const std::string& typed, const CivilDate& today,
                           DateParse& out) {
    if (w.size() != 2 && w.size() != 3) {
        return false;
    }
    int month = MonthFromWord(w[0]);
    std::string dayText = w[1];
    if (month == 0) {
        month = MonthFromWord(w[1]);
        dayText = w[0];
    }
    if (month == 0 || !AllDigits(dayText) || dayText.size() > 2) {
        return false;
    }
    int year = today.Year;
    if (w.size() == 3) {
        if (!AllDigits(w[2]) || w[2].size() == 3 || w[2].size() == 1) {
            return false;
        }
        year = ExpandYear(std::atoi(w[2].c_str()), w[2].size());
    }
    out = CheckedDate(year, month, std::atoi(dayText.c_str()), typed);
    return true;
}

/// "today", "tomorrow", "yesterday" and a weekday with an optional "next".
bool TryParseNamedDay(const std::vector<std::string>& w, const CivilDate& today, DateParse& out) {
    if (w.size() == 1) {
        static const char* const kToday[] = {"today", "tod", "now"};
        static const char* const kTomorrow[] = {"tomorrow", "tmr", "tmrw", "tom"};
        static const char* const kYesterday[] = {"yesterday", "yday"};
        for (const char* k : kToday) {
            if (w[0] == k) {
                out = DateSuccess(today);
                return true;
            }
        }
        for (const char* k : kTomorrow) {
            if (w[0] == k) {
                out = DateSuccess(AddDays(today, 1));
                return true;
            }
        }
        for (const char* k : kYesterday) {
            if (w[0] == k) {
                out = DateSuccess(AddDays(today, -1));
                return true;
            }
        }
    }
    const bool hasNext = w.size() == 2 && w[0] == "next";
    if (w.size() != 1 && !hasNext) {
        return false;
    }
    const int target = WeekdayFromWord(w.back());
    if (target < 0) {
        return false;
    }
    const int ahead = (target - WeekdayOf(today) + 7) % 7;
    out = DateSuccess(AddDays(today, ahead == 0 ? 7 : ahead));
    return true;
}

/// Days per offset unit word; months are handled apart. 0 when the word is not an offset unit.
int OffsetUnitDays(const std::string& word, bool& isMonth) {
    isMonth = word == "mo" || word == "month" || word == "months";
    if (isMonth) {
        return 0;
    }
    if (word == "d" || word == "day" || word == "days") {
        return 1;
    }
    if (word == "w" || word == "wk" || word == "week" || word == "weeks") {
        return 7;
    }
    return 0;
}

CivilDate AddMonths(const CivilDate& d, int months) {
    long long index = static_cast<long long>(d.Year) * 12 + (d.Month - 1) + months;
    CivilDate out;
    out.Year = static_cast<int>(index / 12);
    out.Month = static_cast<int>(index % 12) + 1;
    const int dim = TrackerDateTimePure::DaysInMonth(out.Year, out.Month);
    out.Day = d.Day > dim ? dim : d.Day;
    return out;
}

/// Splits "+3d" / "-2w" / "3 days" style text into a count and a unit word. False on any other shape.
bool SplitOffset(const std::string& s, int& count, std::string& unit) {
    std::size_t pos = 0;
    std::size_t digits = 0;
    count = 0;
    while (pos < s.size() && IsDigit(s[pos])) {
        if (++digits > 5) {
            return false;
        }
        count = count * 10 + (s[pos] - '0');
        ++pos;
    }
    if (digits == 0) {
        return false;
    }
    SkipSpaces(s, pos);
    unit = ReadWord(s, pos);
    return pos == s.size() && !unit.empty();
}

/// "+3d", "-2w", "+1mo", "in 3 days", "3 days ago".
bool TryParseOffset(const std::string& lower, const CivilDate& today, DateParse& out) {
    std::string body = lower;
    int sign = 0;
    if (!body.empty() && (body[0] == '+' || body[0] == '-')) {
        sign = body[0] == '+' ? 1 : -1;
        body = TrimCopyAsciiWhitespace(body.substr(1));
    } else if (body.compare(0, 3, "in ") == 0) {
        sign = 1;
        body = TrimCopyAsciiWhitespace(body.substr(3));
    } else if (body.size() > 4 && body.compare(body.size() - 4, 4, " ago") == 0) {
        sign = -1;
        body = TrimCopyAsciiWhitespace(body.substr(0, body.size() - 4));
    }
    int count = 0;
    std::string unit;
    if (sign == 0 || !SplitOffset(body, count, unit)) {
        return false;
    }
    bool isMonth = false;
    const int perUnit = OffsetUnitDays(unit, isMonth);
    if (isMonth) {
        out = count > kMaxOffsetDays / 30 ? DateFailure(ParseMessageId::DateYearRange, lower)
                                          : DateSuccess(AddMonths(today, sign * count));
        return true;
    }
    if (perUnit == 0) {
        return false;
    }
    if (static_cast<long long>(count) * perUnit > kMaxOffsetDays) {
        out = DateFailure(ParseMessageId::DateYearRange, lower);
        return true;
    }
    out = DateSuccess(AddDays(today, sign * count * perUnit));
    return true;
}

} // namespace

int WeekdayOf(const CivilDate& d) {
    const long long days = DaysFromCivil(d.Year, d.Month, d.Day);
    // 1970-01-01 was a Thursday (4).
    const long long w = (days + 4) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

CivilDate AddDays(const CivilDate& d, int days) { return CivilFromDays(DaysFromCivil(d.Year, d.Month, d.Day) + days); }

std::string FormatIsoDate(const CivilDate& d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.Year, d.Month, d.Day);
    return std::string(buf);
}

DateParse ParseDateInput(const std::string& input, const CivilDate& today, DateOrder order) {
    const std::string s = TrimCopyAsciiWhitespace(input);
    if (s.empty()) {
        DateParse r;
        r.Empty = true;
        return r;
    }
    DateParse r;
    if (!IsValidDate(today.Year, today.Month, today.Day)) {
        return DateFailure(ParseMessageId::DateUnrecognised, s); // a caller bug; never guess a "today"
    }
    if (TryParseNumericDate(s, today, order, r)) {
        return r;
    }
    const std::string lower = ToLowerAsciiCopy(s);
    if (TryParseOffset(lower, today, r)) {
        return r;
    }
    const std::vector<std::string> words = SplitWords(s);
    if (TryParseNamedDay(words, today, r) || TryParseMonthNameDate(words, s, today, r)) {
        return r;
    }
    return DateFailure(ParseMessageId::DateUnrecognised, s);
}

// Issue references

namespace {

bool IsKeyPrefixChar(char c) { return IsAsciiAlpha(c) || IsDigit(c) || c == '_'; }

/// "owner/repo#12": one '/', one '#', a positive number after it, no spaces.
bool LooksLikeGitHubIssueKey(const std::string& s) {
    const std::size_t slash = s.find('/');
    const std::size_t hash = s.find('#');
    if (slash == std::string::npos || hash == std::string::npos || slash == 0 || hash <= slash + 1 ||
        hash + 1 >= s.size() || s.find('/', slash + 1) != std::string::npos ||
        s.find('#', hash + 1) != std::string::npos) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (IsSpace(s[i])) {
            return false;
        }
        if (i > hash && !IsDigit(s[i])) {
            return false;
        }
    }
    return s[hash + 1] != '0';
}

/// The value of `name` in a query string ("a=1&selectedIssue=X-1"), or empty.
std::string QueryParam(const std::string& query, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, name) == 0) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return std::string();
}

bool IsGitHubHost(const std::string& host) { return host == "github.com" || host == "www.github.com"; }

/// The issue key a URL names, or empty.
std::string KeyFromUrl(const UrlParts& url) {
    const std::vector<std::string>& seg = url.Segments;
    if (IsGitHubHost(url.Host) && seg.size() >= 4 && (seg[2] == "issues" || seg[2] == "pull" || seg[2] == "pulls")) {
        const std::string key = seg[0] + "/" + seg[1] + "#" + seg[3];
        return LooksLikeGitHubIssueKey(key) ? key : std::string();
    }
    const std::string selected = QueryParam(url.Query, "selectedIssue");
    if (LooksLikeProjectIssueKey(selected)) {
        return ToUpperAsciiCopy(selected);
    }
    for (std::size_t i = 0; i + 1 < seg.size(); ++i) {
        if ((seg[i] == "browse" || seg[i] == "issue") && LooksLikeProjectIssueKey(seg[i + 1])) {
            return ToUpperAsciiCopy(seg[i + 1]);
        }
    }
    if (EndsWithAscii(url.Host, "linear.app")) {
        return std::string(); // a Linear URL names its key after "/issue/"; anything else is a list page
    }
    for (std::size_t i = seg.size(); i-- > 0;) {
        if (LooksLikeProjectIssueKey(seg[i])) {
            return ToUpperAsciiCopy(seg[i]);
        }
    }
    return std::string();
}

} // namespace

bool LooksLikeUrl(const std::string& text) {
    const std::string lower = ToLowerAsciiCopy(text.substr(0, 8));
    if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0) {
        return true;
    }
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos || slash == 0) {
        return false;
    }
    bool sawDot = false;
    for (std::size_t i = 0; i < slash; ++i) {
        const char c = text[i];
        if (c == '.') {
            sawDot = true;
        } else if (!IsAsciiAlpha(c) && !IsDigit(c) && c != '-' && c != ':') {
            return false;
        }
    }
    return sawDot && text[slash - 1] != '.';
}

bool LooksLikeProjectIssueKey(const std::string& text) {
    const std::size_t dash = text.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= text.size() || !IsAsciiAlpha(text[0])) {
        return false;
    }
    for (std::size_t i = 0; i < dash; ++i) {
        if (!IsKeyPrefixChar(text[i])) {
            return false;
        }
    }
    for (std::size_t i = dash + 1; i < text.size(); ++i) {
        if (!IsDigit(text[i])) {
            return false;
        }
    }
    return true;
}

IssueReferenceParse ParseIssueReference(const std::string& input) {
    IssueReferenceParse r;
    std::string s = TrimCopyAsciiWhitespace(input);
    if (s.size() >= 2 && s.front() == '<' && s.back() == '>') {
        s = TrimCopyAsciiWhitespace(s.substr(1, s.size() - 2)); // a URL pasted from a mail client
    }
    if (s.empty()) {
        r.Empty = true;
        return r;
    }
    if (LooksLikeProjectIssueKey(s)) {
        r.Ok = true;
        r.Key = ToUpperAsciiCopy(s);
        return r;
    }
    if (LooksLikeGitHubIssueKey(s)) {
        r.Ok = true;
        r.Key = s;
        return r;
    }
    if (LooksLikeUrl(s)) {
        r.Key = KeyFromUrl(SplitUrl(s));
        r.Ok = !r.Key.empty();
        r.FromUrl = r.Ok;
        if (!r.Ok) {
            r.Message = MakeParseMessage(ParseMessageId::IssueRefNoKeyInUrl, std::string(), 0, s.size());
        }
        return r;
    }
    r.Message = MakeParseMessage(ParseMessageId::IssueRefNotAKey, s, 0, s.size());
    return r;
}

} // namespace TrackerParseExplain
