#include "GitHubClientHelpers.h"

#include "TrackerDateTimePure.h" // For TimeGmPortable — cross-platform timegm.

#include <cctype>
#include <ctime>
#include <cstring>
#include <string>

namespace GitHubClientHelpers {

namespace {

bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
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
    if (iso8601[4] != '-' || iso8601[7] != '-' || iso8601[10] != 'T' ||
        iso8601[13] != ':' || iso8601[16] != ':' || iso8601[19] != 'Z') {
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
        !digitsTo(iso8601, 11, 2, hour) || !digitsTo(iso8601, 14, 2, minute) ||
        !digitsTo(iso8601, 17, 2, second)) {
        outError = "ISO-8601 timestamp contains non-digit in numeric field.";
        return false;
    }
    if (month < 1 || month > 12) {
        outError = "ISO-8601 month out of range.";
        return false;
    }
    if (day < 1 || day > 31) {
        outError = "ISO-8601 day out of range.";
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

} // namespace GitHubClientHelpers
