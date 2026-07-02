#include "LinearClientHelpers.h"

#include <cctype>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace linear {

const char* const kDefaultLinearApiUrl = "https://api.linear.app/graphql";

namespace {

// Parse a signed long from a lowercased-header map, returning `fallback` when the
// key is absent or the value is not a clean integer. Kept local — the only
// callers are the rate-limit parser below.
long ParseLongOr(const std::map<std::string, std::string>& headers, const char* key, long fallback) {
    std::map<std::string, std::string>::const_iterator it = headers.find(key);
    if (it == headers.end() || it->second.empty()) {
        return fallback;
    }
    const std::string& s = it->second;
    std::size_t i = 0;
    bool negative = false;
    if (s[0] == '+' || s[0] == '-') {
        negative = (s[0] == '-');
        i = 1;
    }
    if (i >= s.size()) {
        return fallback;
    }
    // CPP_CODE_AUDIT.md #18: checked accumulation — `long` is 32-bit on Windows and these
    // headers (x-ratelimit-*, x-complexity) are server-controlled; unchecked `value*10+digit`
    // is signed-overflow UB on a hostile/buggy header value. Accumulate the unsigned magnitude
    // (not `long` directly) so the negative side's extra representable value (LONG_MIN, whose
    // magnitude is LONG_MAX+1) doesn't get rejected as a false overflow.
    using UMag = unsigned long;
    const UMag kMaxMagnitude = negative ? static_cast<UMag>((std::numeric_limits<long>::max)()) + 1UL
                                        : static_cast<UMag>((std::numeric_limits<long>::max)());
    UMag magnitude = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return fallback;
        }
        const UMag digit = static_cast<UMag>(c - '0');
        if (magnitude > kMaxMagnitude / 10 || (magnitude == kMaxMagnitude / 10 && digit > kMaxMagnitude % 10)) {
            return fallback;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) {
        return static_cast<long>(magnitude);
    }
    if (magnitude == kMaxMagnitude) {
        return (std::numeric_limits<long>::min)();
    }
    return -static_cast<long>(magnitude);
}

} // namespace

bool ParseLinearIssueKey(const std::string& issueKey, ParsedLinearIssueKey& out) {
    // Split on the single '-' separating the team key from the issue number.
    const std::string::size_type dash = issueKey.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= issueKey.size()) {
        return false;
    }
    const std::string teamKey = issueKey.substr(0, dash);
    const std::string numberPart = issueKey.substr(dash + 1);

    // Team key: first char ASCII letter, remaining ASCII letters/digits.
    if (!std::isalpha(static_cast<unsigned char>(teamKey[0]))) {
        return false;
    }
    for (std::string::size_type i = 1; i < teamKey.size(); ++i) {
        if (!std::isalnum(static_cast<unsigned char>(teamKey[i]))) {
            return false;
        }
    }

    // Number: all digits, parsed as a positive int64. CPP_CODE_AUDIT.md #18: checked
    // accumulation — an attacker-supplied issue key (e.g. from a deep-link or webhook
    // payload) with an unbounded digit run would otherwise hit signed-overflow UB before
    // the value ever reaches the GraphQL `number.eq` filter.
    std::int64_t number = 0;
    for (std::string::size_type i = 0; i < numberPart.size(); ++i) {
        const char c = numberPart[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const std::int64_t digit = c - '0';
        constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
        if (number > kMax / 10 || (number == kMax / 10 && digit > kMax % 10)) {
            return false;
        }
        number = number * 10 + digit;
    }
    if (number <= 0) {
        return false;
    }

    out.TeamKey = teamKey;
    out.Number = number;
    return true;
}

std::string FormatLinearIssueKey(const std::string& teamKey, std::int64_t number) {
    return teamKey + "-" + std::to_string(number);
}

std::string NormalizeLinearApiUrl(const std::string& apiUrl) {
    if (apiUrl.empty()) {
        return kDefaultLinearApiUrl;
    }
    std::string normalized = apiUrl;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        return kDefaultLinearApiUrl;
    }
    return normalized;
}

Result<bool, std::string> IsValidLinearApiUrl(const std::string& apiUrl) {
    if (apiUrl.empty()) {
        // Empty is acceptable — the default endpoint is used.
        return Result<bool, std::string>::Ok(true);
    }
    const std::string httpsPrefix = "https://";
    const std::string httpPrefix = "http://";
    if (apiUrl.compare(0, httpPrefix.size(), httpPrefix) == 0) {
        return Result<bool, std::string>::Err("Linear API URL must use https://, not http://");
    }
    if (apiUrl.compare(0, httpsPrefix.size(), httpsPrefix) != 0) {
        return Result<bool, std::string>::Err("Linear API URL must be an https:// URL");
    }
    if (apiUrl.size() <= httpsPrefix.size()) {
        return Result<bool, std::string>::Err("Linear API URL is missing a host");
    }
    return Result<bool, std::string>::Ok(true);
}

std::string BuildGraphQLBody(const std::string& query, const nlohmann::json& variables) {
    nlohmann::json body;
    body["query"] = query;
    body["variables"] = variables.is_null() ? nlohmann::json::object() : variables;
    return body.dump();
}

std::string BuildLinearAuthHeaderValue(const std::string& apiKey) {
    // Linear personal API keys are sent raw — NO "Bearer " prefix.
    return apiKey;
}

bool LinearResponseHasErrors(const nlohmann::json& parsed, std::string& outMessage) {
    outMessage.clear();
    if (!parsed.is_object()) {
        return false;
    }
    nlohmann::json::const_iterator it = parsed.find("errors");
    if (it == parsed.end() || !it->is_array() || it->empty()) {
        return false;
    }
    std::vector<std::string> messages;
    for (nlohmann::json::const_iterator e = it->begin(); e != it->end(); ++e) {
        if (e->is_object()) {
            nlohmann::json::const_iterator msg = e->find("message");
            if (msg != e->end() && msg->is_string()) {
                messages.push_back(msg->get<std::string>());
                continue;
            }
        }
        messages.push_back(e->dump());
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            outMessage += "; ";
        }
        outMessage += messages[i];
    }
    return !outMessage.empty();
}

std::string ExtractLinearErrorMessage(int httpStatus, const std::string& body) {
    const std::string fallback = "HTTP " + std::to_string(httpStatus);
    if (body.empty()) {
        return fallback;
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return fallback;
    }
    std::string message;
    if (LinearResponseHasErrors(parsed, message)) {
        return message;
    }
    return fallback;
}

LinearRateLimit ParseLinearRateLimitHeaders(const std::map<std::string, std::string>& headersLower) {
    LinearRateLimit rl;
    rl.RequestsLimit = ParseLongOr(headersLower, "x-ratelimit-requests-limit", -1);
    rl.RequestsRemaining = ParseLongOr(headersLower, "x-ratelimit-requests-remaining", -1);
    rl.RequestsResetUnixSec = ParseLongOr(headersLower, "x-ratelimit-requests-reset", -1);
    rl.ComplexityLimit = ParseLongOr(headersLower, "x-ratelimit-complexity-limit", -1);
    rl.ComplexityRemaining = ParseLongOr(headersLower, "x-ratelimit-complexity-remaining", -1);
    rl.Complexity = ParseLongOr(headersLower, "x-complexity", -1);
    return rl;
}

LinearRequestAuth ResolveLinearRequestAuth(const std::string& cfgApiUrl, const std::string& cfgApiKey,
                                           const std::string& fallbackApiUrl) {
    LinearRequestAuth auth;
    // Live key is authoritative — an empty live key means the user cleared it.
    auth.ApiKey = cfgApiKey;
    const std::string& chosenUrl = !cfgApiUrl.empty() ? cfgApiUrl : fallbackApiUrl;
    auth.ApiUrl = NormalizeLinearApiUrl(chosenUrl);
    return auth;
}

} // namespace linear
} // namespace smatchet
