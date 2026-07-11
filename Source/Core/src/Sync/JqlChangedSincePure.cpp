#include "Sync/JqlChangedSincePure.h"

#include <cctype>
#include <cstdio>

namespace smatchet {

namespace {

// Trim ASCII whitespace from both ends. Returns the trimmed substring.
std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// True when `c` is JQL token whitespace.
bool IsWs(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Case-insensitive ASCII compare of `s[at..at+len)` against `lit` (lit is lower-case).
bool MatchesCi(const std::string& s, std::size_t at, const char* lit, std::size_t len) {
    if (at + len > s.size()) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[at + i])));
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

// Index of the top-level trailing `ORDER BY` (case-insensitive, flexible inner
// whitespace), or std::string::npos. JQL has no subqueries, so the rightmost
// `order ... by` at column boundary is the sort clause. Requires a whitespace (or
// start) boundary before `order` so a token like "reorder" never matches.
//
// CPP_CODE_AUDIT.md #33 (JQL "order by" matched inside quoted strings): the scan
// tracks single/double-quoted string-literal spans (backslash-escape aware, JQL's
// own escaping) and skips matching inside them — a view JQL like
// `summary ~ "sort order by date" AND ...` must not have its quoted text mistaken
// for the real ORDER BY clause. Mirrors the token-aware fix already shipped for
// this exact class in GitHubQueryFromJql.cpp's StripOrderByClause.
std::size_t FindTrailingOrderBy(const std::string& s) {
    std::size_t found = std::string::npos;
    char quoteChar = '\0';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoteChar != '\0') {
            if (c == '\\' && i + 1 < s.size()) {
                ++i; // skip the escaped character — it can't close the quote
            } else if (c == quoteChar) {
                quoteChar = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quoteChar = c;
            continue;
        }
        if (i + 5 > s.size() || !MatchesCi(s, i, "order", 5)) {
            continue;
        }
        if (i > 0 && !IsWs(s[i - 1])) {
            continue; // not a word boundary (e.g. "reorder")
        }
        std::size_t j = i + 5;
        if (j >= s.size() || !IsWs(s[j])) {
            continue; // need whitespace between "order" and "by"
        }
        while (j < s.size() && IsWs(s[j])) {
            ++j;
        }
        if (!MatchesCi(s, j, "by", 2)) {
            continue;
        }
        const std::size_t after = j + 2;
        if (after < s.size() && !IsWs(s[after])) {
            continue; // "byword" — not the keyword
        }
        found = i; // keep scanning; take the rightmost match
    }
    return found;
}

} // namespace

std::string WrapJqlChangedWithin(const std::string& baseJql, int minutes) {
    const int m = minutes > 0 ? minutes : 1;
    const std::string clause = "updated >= -" + std::to_string(m) + "m";

    const std::string trimmed = Trim(baseJql);
    if (trimmed.empty()) {
        return clause;
    }

    const std::size_t orderPos = FindTrailingOrderBy(trimmed);
    if (orderPos == std::string::npos) {
        return "(" + trimmed + ") AND " + clause;
    }

    const std::string body = Trim(trimmed.substr(0, orderPos));
    const std::string orderBy = Trim(trimmed.substr(orderPos));
    if (body.empty()) {
        // Degenerate: base was ORDER-BY-only. Keep the window, re-append the sort.
        return clause + " " + orderBy;
    }
    return "(" + body + ") AND " + clause + " " + orderBy;
}

namespace {

// Howard Hinnant's civil-from-days: days since 1970-01-01 -> (year, month, day).
// Pure integer math, valid across the proleptic Gregorian calendar; no locale/TZ.
void CivilFromDays(std::int64_t z, int& year, unsigned& month, unsigned& day) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11]
    day = doy - (153 * mp + 2) / 5 + 1;                                         // [1, 31]
    month = mp < 10 ? mp + 3 : mp - 9;                                          // [1, 12]
    year = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (month <= 2 ? 1 : 0);
}

} // namespace

std::string IsoSinceFromWindow(std::int64_t nowUnix, std::int64_t windowSeconds) {
    const std::int64_t window = windowSeconds > 0 ? windowSeconds : 0;
    std::int64_t since = nowUnix - window;
    if (since < 0) {
        since = 0;
    }
    const std::int64_t days = since / 86400;
    const std::int64_t secOfDay = since % 86400;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(days, year, month, day);

    const int hh = static_cast<int>(secOfDay / 3600);
    const int mm = static_cast<int>((secOfDay % 3600) / 60);
    const int ss = static_cast<int>(secOfDay % 60);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02dZ", year, month, day, hh, mm, ss);
    return std::string(buf);
}

} // namespace smatchet
