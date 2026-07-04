#include "GitHubClientHelpers.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio> // snprintf / sscanf — some toolchains don't transitively include via <ctime>/<sstream>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>

namespace smatchet {
namespace github {

bool ParseGitHubIssueKey(const std::string& issueKey, ParsedIssueKey& out) {
    // Format: <owner>/<repo>#<positive-int>. Owner + repo: GitHub allows
    // alphanumeric, hyphen, underscore, dot. Number: 1+ digits, > 0.
    if (issueKey.empty()) {
        return false;
    }
    const std::size_t slash = issueKey.find('/');
    if (slash == std::string::npos || slash == 0) {
        return false;
    }
    const std::size_t hash = issueKey.find('#', slash + 1);
    if (hash == std::string::npos || hash == slash + 1 || hash + 1 >= issueKey.size()) {
        return false;
    }
    const std::string owner = issueKey.substr(0, slash);
    const std::string repo = issueKey.substr(slash + 1, hash - slash - 1);
    const std::string numStr = issueKey.substr(hash + 1);

    auto valid_seg = [](const std::string& s) {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
                return false;
            }
        }
        return true;
    };
    if (!valid_seg(owner) || !valid_seg(repo)) {
        return false;
    }
    for (char c : numStr) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        const std::int64_t n = std::stoll(numStr);
        if (n <= 0) {
            return false;
        }
        out.Owner = owner;
        out.Repo = repo;
        out.Number = n;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseGitHubCommitKey(const std::string& commitKey, ParsedCommitKey& out) {
    // Format: <owner>/<repo>@<hex-sha>. Owner + repo charset mirrors
    // ParseGitHubIssueKey (alphanumeric, hyphen, underscore, dot). SHA: 7..40
    // hex chars (abbreviated through full 40-char git object id).
    if (commitKey.empty()) {
        return false;
    }
    const std::size_t slash = commitKey.find('/');
    if (slash == std::string::npos || slash == 0) {
        return false;
    }
    const std::size_t at = commitKey.find('@', slash + 1);
    if (at == std::string::npos || at == slash + 1 || at + 1 >= commitKey.size()) {
        return false;
    }
    const std::string owner = commitKey.substr(0, slash);
    const std::string repo = commitKey.substr(slash + 1, at - slash - 1);
    const std::string sha = commitKey.substr(at + 1);

    auto valid_seg = [](const std::string& s) {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
                return false;
            }
        }
        return true;
    };
    if (!valid_seg(owner) || !valid_seg(repo)) {
        return false;
    }
    if (sha.size() < 7 || sha.size() > 40) {
        return false;
    }
    for (char c : sha) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    out.Owner = owner;
    out.Repo = repo;
    out.Sha = sha;
    return true;
}

std::string BuildCommitListUrlSuffix(const std::string& owner, const std::string& repo, int perPage) {
    if (perPage < 1) {
        perPage = 1;
    } else if (perPage > 100) {
        perPage = 100;
    }
    std::ostringstream out;
    out << "/repos/" << owner << "/" << repo << "/commits?per_page=" << perPage;
    return out.str();
}

std::string BuildCommitBrowseUrlSuffix(const std::string& owner, const std::string& repo, const std::string& sha) {
    std::ostringstream out;
    out << "/" << owner << "/" << repo << "/commit/" << sha;
    return out.str();
}

namespace {
std::string IsoZuluFromUnixSec(std::int64_t unixSec) {
    std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}
} // namespace

std::string BuildIssueListUrlSuffix(const std::string& owner, const std::string& repo, int perPage,
                                    std::int64_t sinceUnixSec) {
    if (perPage < 1) {
        perPage = 1;
    } else if (perPage > 100) {
        perPage = 100;
    }
    std::ostringstream out;
    out << "/repos/" << owner << "/" << repo << "/issues?per_page=" << perPage << "&state=all";
    if (sinceUnixSec > 0) {
        out << "&since=" << IsoZuluFromUnixSec(sinceUnixSec);
    }
    return out.str();
}

std::string BuildIssuePatchUrlSuffix(const std::string& owner, const std::string& repo, std::int64_t number) {
    std::ostringstream out;
    out << "/repos/" << owner << "/" << repo << "/issues/" << number;
    return out.str();
}

Result<bool, std::string> IsValidGitHubBaseUrl(const std::string& baseUrl) {
    // Strict host check — accept exactly "https://api.github.com" (cloud) or
    // any URL whose path is exactly "/api/v3" on an arbitrary https host (Enterprise).
    // Reject any other shape so downstream URL composition lands on a working REST root.
    if (baseUrl.empty()) {
        return Result<bool, std::string>::Err("GitHub base URL is empty");
    }
    if (baseUrl.compare(0, 8, "https://") != 0) {
        return Result<bool, std::string>::Err("GitHub base URL must start with https://");
    }
    if (baseUrl.back() == '/') {
        return Result<bool, std::string>::Err("GitHub base URL must not have a trailing slash");
    }
    // Exact cloud match.
    if (baseUrl == "https://api.github.com") {
        return Result<bool, std::string>::Ok(true);
    }
    // Enterprise — must end with /api/v3 + host portion non-empty.
    const std::string apiSuffix = "/api/v3";
    if (baseUrl.size() <= 8 + apiSuffix.size()) {
        return Result<bool, std::string>::Err(
            "GitHub base URL must be 'https://api.github.com' or 'https://<host>/api/v3'");
    }
    if (baseUrl.compare(baseUrl.size() - apiSuffix.size(), apiSuffix.size(), apiSuffix) != 0) {
        return Result<bool, std::string>::Err("GitHub Enterprise base URL must end with '/api/v3' (got '" + baseUrl +
                                              "')");
    }
    // Verify the host portion (between 'https://' and '/api/v3') is non-empty + contains no
    // additional path segments.
    const std::string host = baseUrl.substr(8, baseUrl.size() - 8 - apiSuffix.size());
    if (host.empty() || host.find('/') != std::string::npos) {
        return Result<bool, std::string>::Err("GitHub Enterprise base URL has invalid host portion: '" + host + "'");
    }
    return Result<bool, std::string>::Ok(true);
}

namespace {

// Comma-split a CSV string into a JSON array of trimmed, non-blank tokens.
// Returns an empty array when nothing remains (caller decides whether to emit).
nlohmann::json SplitCsvToJsonArray(const std::string& csv) {
    nlohmann::json arr = nlohmann::json::array();
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? csv.size() : comma;
        std::string token = csv.substr(start, end - start);
        const std::size_t first = token.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const std::size_t last = token.find_last_not_of(" \t\r\n");
            arr.push_back(token.substr(first, last - first + 1));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return arr;
}

} // namespace

Result<nlohmann::json, std::string> BuildGitHubCreatePayload(const std::string& summary, const std::string& body,
                                                             const std::string& labelsCsv,
                                                             const std::string& assigneesCsv, const std::string& owner,
                                                             const std::string& repo) {
    if (summary.empty()) {
        return Result<nlohmann::json, std::string>::Err("GitHub issue create requires a non-empty title (summary)");
    }
    if (owner.empty() || repo.empty()) {
        return Result<nlohmann::json, std::string>::Err("GitHub issue create requires a target repo (owner/repo)");
    }
    nlohmann::json out = nlohmann::json::object();
    out["title"] = summary;
    if (!body.empty()) {
        out["body"] = body;
    }
    const nlohmann::json labels = SplitCsvToJsonArray(labelsCsv);
    if (!labels.empty()) {
        out["labels"] = labels;
    }
    const nlohmann::json assignees = SplitCsvToJsonArray(assigneesCsv);
    if (!assignees.empty()) {
        out["assignees"] = assignees;
    }
    // Out-of-band target so CreateIssue forms the URL without re-parsing ProjectKey.
    // Stripped (erase) before the body is POSTed.
    nlohmann::json target = nlohmann::json::object();
    target["owner"] = owner;
    target["repo"] = repo;
    out["__target"] = target;
    return Result<nlohmann::json, std::string>::Ok(std::move(out));
}

std::string FormatGitHubIssueKey(const std::string& owner, const std::string& repo, std::int64_t number) {
    std::ostringstream out;
    out << owner << "/" << repo << "#" << number;
    return out.str();
}

std::string ExtractGitHubErrorMessage(int httpStatus, const std::string& body) {
    if (!body.empty()) {
        try {
            // Bounded parse — the try can't catch a depth-bomb's destructor-time SIGSEGV
            // (audit: unbounded-recursion-DoS). Discarded on failure → !is_object() path below.
            const auto j = smatchet::json_safe::ParseBoundedOrDiscarded(body);
            if (j.is_object() && j.contains("message") && j["message"].is_string()) {
                return j["message"].get<std::string>();
            }
        } catch (const std::exception&) {
            // Fall through to status fallback.
        }
    }
    std::ostringstream out;
    out << "HTTP " << httpStatus;
    return out.str();
}

Result<std::int64_t, std::string> ParseIso8601ToUnixSec(const std::string& iso8601) {
    if (iso8601.empty()) {
        return Result<std::int64_t, std::string>::Err("empty timestamp");
    }
    // Parse YYYY-MM-DDTHH:MM:SS first, then handle the optional timezone suffix.
    // A prior impl ignored non-zero offsets (e.g. +05:30)
    // and returned wrong epoch. Now: 'Z' → UTC; '+HH:MM' / '-HH:MM' / '+HHMM' / '-HHMM' →
    // adjust the computed time_t accordingly; missing suffix → reject (force callers to
    // disambiguate UTC explicitly).
    int year = 0, mon = 0, day = 0, hour = 0, minute = 0, sec = 0;
    int consumed = 0;
    if (std::sscanf(iso8601.c_str(), "%d-%d-%dT%d:%d:%d%n", &year, &mon, &day, &hour, &minute, &sec, &consumed) != 6) {
        return Result<std::int64_t, std::string>::Err("bad ISO-8601 format: " + iso8601);
    }
    std::string suffix = iso8601.substr(static_cast<std::size_t>(consumed));
    // Strip a fractional-seconds component before classifying the timezone. Jira emits
    // millisecond precision ("...:00.000+0000"); GitHub does not. The leading '.<digits>'
    // is dropped so the tz classifier below sees the bare offset ("+0000") — otherwise the
    // '.' falls through to the "missing timezone suffix" reject. Sub-second precision is
    // discarded by design (epoch-seconds resolution).
    if (!suffix.empty() && suffix.front() == '.') {
        std::size_t i = 1;
        while (i < suffix.size() && std::isdigit(static_cast<unsigned char>(suffix[i])) != 0) {
            ++i;
        }
        // A bare '.' with no following digit ("...:00.Z", "...:00.+0000") is malformed: reject
        // rather than stripping the dot and silently re-accepting the remainder as the tz suffix.
        if (i == 1) {
            return Result<std::int64_t, std::string>::Err("malformed fractional seconds in ISO-8601: " + iso8601);
        }
        suffix = suffix.substr(i);
    }
    std::int64_t offsetSec = 0;
    if (suffix == "Z" || suffix == "+00:00" || suffix == "-00:00" || suffix == "+0000" || suffix == "-0000") {
        offsetSec = 0;
    } else if (!suffix.empty() && (suffix.front() == '+' || suffix.front() == '-')) {
        const int sign = (suffix.front() == '-') ? -1 : 1;
        int offH = 0, offM = 0;
        // Tolerate `+HH:MM` and `+HHMM` shapes (both standard ISO-8601).
        if (std::sscanf(suffix.c_str() + 1, "%d:%d", &offH, &offM) != 2 &&
            std::sscanf(suffix.c_str() + 1, "%2d%2d", &offH, &offM) != 2) {
            return Result<std::int64_t, std::string>::Err("unrecognised timezone offset: '" + suffix + "' in " +
                                                          iso8601);
        }
        // Bounds-check the parsed offset — `%d:%d` accepts
        // arbitrary integers (e.g. `+53:99`). Max real-world offset is `+14:00` (Pacific/Kiritimati).
        if (offH < 0 || offH > 14 || offM < 0 || offM > 59 || (offH == 14 && offM != 0)) {
            return Result<std::int64_t, std::string>::Err(
                "timezone offset out of range (expected ±00:00 to ±14:00): '" + suffix + "' in " + iso8601);
        }
        offsetSec = sign * (static_cast<std::int64_t>(offH) * 3600 + static_cast<std::int64_t>(offM) * 60);
    } else {
        return Result<std::int64_t, std::string>::Err(
            "ISO-8601 timestamp missing timezone suffix (need 'Z' or '\xC2\xB1HH:MM'): " + iso8601);
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = sec;
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) {
        return Result<std::int64_t, std::string>::Err("timegm/mkgmtime rejected: " + iso8601);
    }
    // UTC = local - offset (e.g. 12:00 +05:30 → 06:30 UTC → subtract 5:30 from local epoch).
    return Result<std::int64_t, std::string>::Ok(static_cast<std::int64_t>(t) - offsetSec);
}

GitHubRequestAuth ResolveGitHubRequestAuth(const std::string& cfgBaseUrl, const std::string& cfgPat,
                                           const std::string& fallbackBaseUrl) {
    // Issue #979 — the live cfg PAT wins UNCONDITIONALLY (even when empty): every
    // cfg-bearing call path passes a real user config, so an empty live PAT means the
    // user deliberately cleared the credential and the client must stop sending the
    // ctor snapshot (review 2026-06-07: a non-empty-wins rule kept authenticating with
    // a revoked credential forever — the PAT deliberately has NO fallback). The base
    // URL keeps the fallback chain and defaults to GitHub cloud when both are empty
    // (the same default the ctor applies).
    GitHubRequestAuth out;
    out.Pat = cfgPat;
    if (!cfgBaseUrl.empty()) {
        out.BaseUrl = cfgBaseUrl;
    } else if (!fallbackBaseUrl.empty()) {
        out.BaseUrl = fallbackBaseUrl;
    } else {
        out.BaseUrl = "https://api.github.com";
    }
    return out;
}

} // namespace github
} // namespace smatchet
