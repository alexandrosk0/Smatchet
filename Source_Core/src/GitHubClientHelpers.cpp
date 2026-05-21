#include "GitHubClientHelpers.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
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

bool IsValidGitHubBaseUrl(const std::string& baseUrl, std::string& outError) {
    if (baseUrl.empty()) {
        outError = "GitHub base URL is empty";
        return false;
    }
    if (baseUrl.compare(0, 8, "https://") != 0) {
        outError = "GitHub base URL must start with https://";
        return false;
    }
    if (!baseUrl.empty() && baseUrl.back() == '/') {
        outError = "GitHub base URL must not have a trailing slash";
        return false;
    }
    outError.clear();
    return true;
}

std::string ExtractGitHubErrorMessage(int httpStatus, const std::string& body) {
    if (!body.empty()) {
        try {
            const auto j = nlohmann::json::parse(body);
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

std::int64_t ParseIso8601ToUnixSec(const std::string& iso8601, std::string& outError) {
    if (iso8601.empty()) {
        outError = "empty timestamp";
        return 0;
    }
    // Strict format: YYYY-MM-DDTHH:MM:SS[Z|+00:00]. Tolerant on the offset
    // form; we only consume the date+time and treat as UTC.
    int year = 0, mon = 0, day = 0, hour = 0, minute = 0, sec = 0;
    if (std::sscanf(iso8601.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &minute, &sec) != 6) {
        outError = "bad ISO-8601 format: " + iso8601;
        return 0;
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
        outError = "timegm/mkgmtime rejected: " + iso8601;
        return 0;
    }
    outError.clear();
    return static_cast<std::int64_t>(t);
}

} // namespace github
} // namespace smatchet
