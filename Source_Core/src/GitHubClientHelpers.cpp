#include "GitHubClientHelpers.h"

#include "TrackerDateTimePure.h" // For TimeGmPortable — cross-platform timegm.

#include <cctype>
#include <ctime>
#include <cstring>
#include <sstream>
#include <string>

namespace GitHubClientHelpers {

namespace {

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// Manual int64 → decimal string. Used by the URL-suffix builders so they don't
// pull <sstream> into every consuming TU. Negative numbers format with a leading
// `-`; `ParseGitHubIssueKey` rejects them upstream, so the URL builders should
// never see them in practice.
std::string Int64ToDecimal(std::int64_t n) {
    if (n == 0) {
        return "0";
    }
    const bool negative = n < 0;
    unsigned long long magnitude =
        negative ? (static_cast<unsigned long long>(-(n + 1)) + 1ULL) : static_cast<unsigned long long>(n);
    std::string digits;
    while (magnitude > 0) {
        digits.push_back(static_cast<char>('0' + (magnitude % 10)));
        magnitude /= 10;
    }
    std::string out;
    if (negative) {
        out.push_back('-');
    }
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        out.push_back(*it);
    }
    return out;
}

std::string BuildIssueRootPrefix(const ParsedIssueKey& parsed) {
    std::string out;
    out.reserve(8 + parsed.Owner.size() + 1 + parsed.Repo.size() + 8 + 6);
    out.append("/repos/");
    out.append(parsed.Owner);
    out.push_back('/');
    out.append(parsed.Repo);
    out.append("/issues/");
    out.append(Int64ToDecimal(parsed.Number));
    return out;
}

} // namespace

bool ParseGitHubIssueKey(const std::string& key, ParsedIssueKey& out, std::string& outError) {
    out = ParsedIssueKey{};
    outError.clear();

    if (key.empty()) {
        outError = "GitHub issue key is empty.";
        return false;
    }
    // Reject leading whitespace — owner cannot start with whitespace and we want a strict
    // round-trip-able shape. Trailing characters after the digits are also rejected below.
    if (std::isspace(static_cast<unsigned char>(key.front()))) {
        outError = "GitHub issue key has leading whitespace.";
        return false;
    }

    const std::size_t slash = key.find('/');
    if (slash == std::string::npos) {
        outError = "GitHub issue key missing '/' separator (expected owner/repo#N).";
        return false;
    }
    const std::size_t hash = key.find('#', slash + 1);
    if (hash == std::string::npos) {
        outError = "GitHub issue key missing '#' separator (expected owner/repo#N).";
        return false;
    }

    const std::string owner = key.substr(0, slash);
    const std::string repo = key.substr(slash + 1, hash - slash - 1);
    const std::string numberStr = key.substr(hash + 1);

    if (owner.empty()) {
        outError = "GitHub issue key has empty owner segment.";
        return false;
    }
    if (repo.empty()) {
        outError = "GitHub issue key has empty repo segment.";
        return false;
    }
    if (numberStr.empty()) {
        outError = "GitHub issue key has empty issue number.";
        return false;
    }
    for (char c : numberStr) {
        if (!IsAsciiDigit(c)) {
            outError = "GitHub issue key issue number contains non-digit character.";
            return false;
        }
    }

    // Manual decimal parse — guard against overflow without dragging strtoll error handling.
    // Issue numbers >= 1; GitHub's max in practice is < 2^31 but the field type stays int64.
    std::int64_t n = 0;
    for (char c : numberStr) {
        const int digit = c - '0';
        if (n > (static_cast<std::int64_t>(INT64_MAX) - digit) / 10) {
            outError = "GitHub issue number overflows int64.";
            return false;
        }
        n = n * 10 + digit;
    }
    if (n <= 0) {
        outError = "GitHub issue number must be >= 1.";
        return false;
    }

    out.Owner = owner;
    out.Repo = repo;
    out.Number = n;
    return true;
}

bool ParseGitHubRepoKey(const std::string& query, std::string& outOwner, std::string& outRepo, std::string& outError) {
    outOwner.clear();
    outRepo.clear();
    outError.clear();

    if (query.empty()) {
        outError = "OWNER/REPO query is empty.";
        return false;
    }
    // Strict whitespace rejection — surface the typo, do not silently trim. Mirrors
    // the issue-key parser's leading-whitespace check and extends it to trailing
    // whitespace + any whitespace embedded between owner / repo (which a single
    // `/` separator search would silently accept otherwise).
    for (std::size_t i = 0; i < query.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(query[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            outError = "OWNER/REPO query contains whitespace.";
            return false;
        }
    }

    const std::size_t slash = query.find('/');
    if (slash == std::string::npos) {
        outError = "OWNER/REPO query missing '/' separator.";
        return false;
    }
    if (query.find('/', slash + 1) != std::string::npos) {
        outError = "OWNER/REPO query must have exactly one '/' separator.";
        return false;
    }

    const std::string owner = query.substr(0, slash);
    const std::string repo = query.substr(slash + 1);
    if (owner.empty()) {
        outError = "OWNER/REPO query has empty owner segment.";
        return false;
    }
    if (repo.empty()) {
        outError = "OWNER/REPO query has empty repo segment.";
        return false;
    }

    outOwner = owner;
    outRepo = repo;
    return true;
}

std::string FormatGitHubIssueKey(const std::string& owner, const std::string& repo, std::int64_t number) {
    // Manual int64 → string to avoid bringing <sstream> into the call path. Negative numbers
    // are emitted with a leading `-`; the parser then rejects them on round-trip, surfacing
    // the bug at the caller.
    std::string numStr;
    if (number == 0) {
        numStr = "0";
    } else {
        const bool negative = number < 0;
        // Build the magnitude as unsigned to safely handle INT64_MIN (whose negation overflows).
        unsigned long long magnitude = negative ? (static_cast<unsigned long long>(-(number + 1)) + 1ULL)
                                                : static_cast<unsigned long long>(number);
        std::string digits;
        while (magnitude > 0) {
            digits.push_back(static_cast<char>('0' + (magnitude % 10)));
            magnitude /= 10;
        }
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            numStr.push_back(*it);
        }
        if (negative) {
            numStr.insert(numStr.begin(), '-');
        }
    }
    std::string out;
    out.reserve(owner.size() + 1 + repo.size() + 1 + numStr.size());
    out.append(owner);
    out.push_back('/');
    out.append(repo);
    out.push_back('#');
    out.append(numStr);
    return out;
}

bool ParseIso8601ToUnixSec(const std::string& iso8601, std::int64_t& outUnixSec, std::string& outError) {
    outUnixSec = 0;
    outError.clear();

    // GitHub's API returns the canonical 20-char form: `YYYY-MM-DDTHH:MM:SSZ`. Reject
    // anything else (fractional seconds, alternate offsets) — the agentic-flow comment
    // shape doesn't need sub-second precision, and any deviation likely signals a
    // schema drift we want to surface.
    if (iso8601.size() != 20) {
        outError = "ISO-8601 timestamp is not 20 chars (expected YYYY-MM-DDTHH:MM:SSZ).";
        return false;
    }
    if (iso8601[4] != '-' || iso8601[7] != '-' || iso8601[10] != 'T' || iso8601[13] != ':' || iso8601[16] != ':' ||
        iso8601[19] != 'Z') {
        outError = "ISO-8601 timestamp has malformed separators.";
        return false;
    }
    auto digitsTo = [](const std::string& s, std::size_t start, std::size_t count, int& out) -> bool {
        out = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = s[start + i];
            if (c < '0' || c > '9') {
                return false;
            }
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!digitsTo(iso8601, 0, 4, year) || !digitsTo(iso8601, 5, 2, month) || !digitsTo(iso8601, 8, 2, day) ||
        !digitsTo(iso8601, 11, 2, hour) || !digitsTo(iso8601, 14, 2, minute) || !digitsTo(iso8601, 17, 2, second)) {
        outError = "ISO-8601 timestamp contains non-digit in numeric field.";
        return false;
    }
    if (month < 1 || month > 12) {
        outError = "ISO-8601 month out of range.";
        return false;
    }
    // Per-month day validation. The bare `day <= 31` check that previously lived
    // here accepted nonsense dates like Feb 30 / Apr 31 / Feb 29 in non-leap years
    // and let the underlying `timegm` silently normalise them into the following
    // month, producing a wrong cursor value. Delegate to the shared days-in-month
    // table (TrackerDateTimePure::DaysInMonth) which already encodes the
    // 31 / 28-29 / 31 / 30 / 31 / 30 / 31 / 31 / 30 / 31 / 30 / 31 layout plus
    // the proleptic-Gregorian leap-year rule for February.
    const int maxDay = TrackerDateTimePure::DaysInMonth(year, month);
    if (day < 1 || day > maxDay) {
        outError = "ISO-8601 day out of range for given month.";
        return false;
    }
    if (hour > 23 || minute > 59 || second > 60) { // 60 — allow leap-second per RFC3339.
        outError = "ISO-8601 time component out of range.";
        return false;
    }

    std::tm tmUtc;
    std::memset(&tmUtc, 0, sizeof(tmUtc));
    tmUtc.tm_year = year - 1900;
    tmUtc.tm_mon = month - 1;
    tmUtc.tm_mday = day;
    tmUtc.tm_hour = hour;
    tmUtc.tm_min = minute;
    tmUtc.tm_sec = second;
    const std::time_t t = TrackerDateTimePure::TimeGmPortable(&tmUtc);
    if (t == static_cast<std::time_t>(-1)) {
        outError = "ISO-8601 timestamp failed timegm conversion.";
        return false;
    }
    outUnixSec = static_cast<std::int64_t>(t);
    return true;
}

std::string FormatUnixSecAsIso8601(std::int64_t unixSec) {
    // The scheduled-poll cursor only walks forward in time, so pre-epoch values
    // are not a meaningful "updated since" filter. Clamp to 0 so GitHub sees
    // `1970-01-01T00:00:00Z` instead of a negative-year URL.
    if (unixSec < 0) {
        unixSec = 0;
    }
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm utc;
    std::memset(&utc, 0, sizeof(utc));
#if defined(_WIN32)
    // Windows ships gmtime_s with reversed param order; both POSIX and MSVCR
    // variants are reentrant. The non-_s gmtime returns a pointer to a static
    // buffer — not safe for the worker-thread cursor invocation. gmtime_s
    // returns 0 on success; non-zero indicates EINVAL on an out-of-range time_t
    // (e.g. on 32-bit time_t targets past 2038). Bundle C — fall back to the
    // epoch sentinel so the cursor URL is still well-formed; the caller's
    // wall-clock-now invocation cannot trip this branch on a 64-bit time_t but
    // the defensive check keeps the function total.
    if (gmtime_s(&utc, &t) != 0) {
        return "1970-01-01T00:00:00Z";
    }
#else
    // gmtime_r returns nullptr on failure (same out-of-range causes as the
    // Windows path). Same epoch-sentinel fallback so callers never see an
    // empty / partial timestamp string.
    if (gmtime_r(&t, &utc) == nullptr) {
        return "1970-01-01T00:00:00Z";
    }
#endif
    // Manual 20-char layout (snprintf would format-flag-route through locale on
    // some libc builds). Year is clamped at 9999 — the cursor will not see a
    // larger value in any conceivable runtime.
    int year = utc.tm_year + 1900;
    if (year < 0)
        year = 0;
    if (year > 9999)
        year = 9999;
    const int month = utc.tm_mon + 1;
    const int day = utc.tm_mday;
    const int hour = utc.tm_hour;
    const int minute = utc.tm_min;
    const int second = utc.tm_sec;
    char buf[21];
    buf[0] = static_cast<char>('0' + (year / 1000) % 10);
    buf[1] = static_cast<char>('0' + (year / 100) % 10);
    buf[2] = static_cast<char>('0' + (year / 10) % 10);
    buf[3] = static_cast<char>('0' + year % 10);
    buf[4] = '-';
    buf[5] = static_cast<char>('0' + (month / 10) % 10);
    buf[6] = static_cast<char>('0' + month % 10);
    buf[7] = '-';
    buf[8] = static_cast<char>('0' + (day / 10) % 10);
    buf[9] = static_cast<char>('0' + day % 10);
    buf[10] = 'T';
    buf[11] = static_cast<char>('0' + (hour / 10) % 10);
    buf[12] = static_cast<char>('0' + hour % 10);
    buf[13] = ':';
    buf[14] = static_cast<char>('0' + (minute / 10) % 10);
    buf[15] = static_cast<char>('0' + minute % 10);
    buf[16] = ':';
    buf[17] = static_cast<char>('0' + (second / 10) % 10);
    buf[18] = static_cast<char>('0' + second % 10);
    buf[19] = 'Z';
    buf[20] = '\0';
    return std::string(buf, 20);
}

std::string PercentEncodePathSegment(const std::string& segment) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(segment.size());
    for (unsigned char c : segment) {
        // RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string BuildIssueCommentsSuffix(const ParsedIssueKey& parsed) {
    return BuildIssueRootPrefix(parsed) + "/comments";
}

std::string BuildIssueLabelsSuffix(const ParsedIssueKey& parsed) { return BuildIssueRootPrefix(parsed) + "/labels"; }

std::string BuildIssueLabelRemoveSuffix(const ParsedIssueKey& parsed, const std::string& labelName) {
    return BuildIssueRootPrefix(parsed) + "/labels/" + PercentEncodePathSegment(labelName);
}

std::string BuildIssueAssigneesSuffix(const ParsedIssueKey& parsed) {
    return BuildIssueRootPrefix(parsed) + "/assignees";
}

std::string BuildIssueRootSuffix(const ParsedIssueKey& parsed) { return BuildIssueRootPrefix(parsed); }

nlohmann::json BuildCommentAddBody(const std::string& body) {
    nlohmann::json obj = nlohmann::json::object();
    obj["body"] = body;
    return obj;
}

nlohmann::json BuildLabelAddBody(const std::string& label) {
    // GitHub's POST /labels endpoint accepts either a top-level array of label
    // names or an object `{"labels": [...]}`. The array form is shorter and the
    // shape GitHub's docs lead with — use it.
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back(label);
    return arr;
}

nlohmann::json BuildAssigneeSetBody(const std::string& user) {
    nlohmann::json obj = nlohmann::json::object();
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back(user);
    obj["assignees"] = arr;
    return obj;
}

nlohmann::json BuildStateTransitionBody(const std::string& state) {
    nlohmann::json obj = nlohmann::json::object();
    obj["state"] = state;
    return obj;
}

bool IsValidStateTransitionTarget(const std::string& state) {
    // Enum-strict per agentic-flow-implementation.md § Decisions locked #3.
    return state == "open" || state == "closed";
}

bool IsValidGitHubBaseUrl(const std::string& baseUrl, std::string& outError) {
    outError.clear();
    if (baseUrl.empty()) {
        outError = "GitHub base URL is empty.";
        return false;
    }
    // Case-sensitive `https://` prefix check. Dangerous schemes (`javascript:`,
    // `file:`, `data:`, `gopher:`, plain `http:` over the open internet, etc.)
    // fall through to the explicit rejection so a hostile config write cannot
    // redirect traffic. Trailing slashes on the authority itself are fine; the
    // GitHubClient ctor strips them via StripTrailingSlash.
    const std::string kScheme = "https://";
    if (baseUrl.size() <= kScheme.size() || baseUrl.compare(0, kScheme.size(), kScheme) != 0) {
        outError = "GitHub base URL must start with 'https://' (got '" + baseUrl + "').";
        return false;
    }
    // Reject `https://` with no authority. The substring check above already
    // refuses `https://`-exact via `<=` rather than `<`; an explicit guard
    // mirrors the issue-key parser's "empty owner" message shape.
    if (baseUrl.size() == kScheme.size()) {
        outError = "GitHub base URL is missing host after scheme.";
        return false;
    }
    // Reject whitespace anywhere — same strictness as the OWNER/REPO parser.
    for (std::size_t i = 0; i < baseUrl.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(baseUrl[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            outError = "GitHub base URL contains whitespace.";
            return false;
        }
    }
    return true;
}

bool ShouldRejectAsTooLarge(std::size_t bodyByteLength) {
    return bodyByteLength > kMaxGitHubRequestBodyBytes;
}

bool ExtractGitHubErrorMessage(const std::string& responseBody, std::string& outMessage) {
    outMessage.clear();
    if (responseBody.empty()) {
        return false;
    }
    try {
        const auto parsed = nlohmann::json::parse(responseBody);
        if (parsed.is_object() && parsed.contains("message") && parsed["message"].is_string()) {
            outMessage = parsed["message"].get<std::string>();
            return !outMessage.empty();
        }
    } catch (const nlohmann::json::parse_error&) {
        // Not JSON — fine; caller falls back to the status-code summary.
    }
    return false;
}

} // namespace GitHubClientHelpers
