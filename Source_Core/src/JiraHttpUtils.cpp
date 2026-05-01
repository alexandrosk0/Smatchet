#include "JiraHttpUtils.h"

#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "StringUtil.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

constexpr std::size_t kMaxJiraHttpBodyLogBytes = 65536;
constexpr const char* kJiraUserAgent = "Smatchet/1.0 Jira-Client";

// Redact URL query for logging: keeps scheme://host/path, drops ?query and #fragment.
std::string RedactUrlForLog(const std::string& url) {
    const size_t q = url.find_first_of("?#");
    if (q == std::string::npos) {
        return url;
    }
    return url.substr(0, q) + "?[redacted]";
}

void LogJiraHttpResult(const char* method, const std::string& url, const cpr::Response& response) {
    LOG_DEBUG("JiraClient: %s %s -> HTTP %d (%zu bytes)", method, RedactUrlForLog(url).c_str(),
              static_cast<int>(response.status_code), response.text.size());
    if (!Logger::Instance().GetLogJiraHttpBodies()) {
        return;
    }
    if (!Logger::Instance().ShouldLog(LogLevel::Trace)) {
        return;
    }
    std::string body = response.text;
    std::string suffix;
    if (body.size() > kMaxJiraHttpBodyLogBytes) {
        body.resize(kMaxJiraHttpBodyLogBytes);
        suffix = "\n[truncated…]";
    }
    Logger::Instance().Log(LogLevel::Trace, std::string("JiraClient: ") + method + " response body (" +
                                                RedactUrlForLog(url) + "):\n" + body + suffix);
}

// Simple URL-encoder that is C++14 friendly.
std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : value) {
        // Unreserved characters according to RFC 3986
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}

// Base64 encode (RFC 4648) so we can send Authorization exactly like PowerShell/curl.
std::string Base64Encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(in[i]);
        const unsigned char b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned char c = (i + 2 < in.size()) ? static_cast<unsigned char>(in[i + 2]) : 0u;
        out += table[a >> 2];
        out += table[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? table[c & 63] : '=';
    }
    return out;
}

std::string NormalizeBaseUrl(const std::string& domain) {
    std::string base = domain;
    if (base.find("http://") != 0 && base.find("https://") != 0) {
        base = "https://" + base;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

bool EnsureJiraAuthConfig(const JiraConfig& cfg, std::string& outError) {
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        LOG_WARN("JiraClient: %s (domain_set=%d token_set=%d)", outError.c_str(), cfg.Domain.empty() ? 0 : 1,
                 cfg.ApiToken.empty() ? 0 : 1);
        return false;
    }
    return true;
}

cpr::Header BuildJiraHeaders(const JiraConfig& cfg, bool includeJsonContentType) {
    cpr::Header headers{{"Accept", "application/json"},
                        {"Authorization", BuildJiraBasicAuthHeader(cfg)},
                        {"User-Agent", kJiraUserAgent}};
    if (includeJsonContentType) {
        headers["Content-Type"] = "application/json";
    }
    return headers;
}

std::string BuildJiraBasicAuthHeader(const JiraConfig& cfg) {
    return "Basic " + Base64Encode(cfg.Email + ":" + cfg.ApiToken);
}

cpr::Response JiraGetLogged(const std::string& url, const cpr::Header& headers) {
    return JiraGetLogged(url, headers, kJiraConnectTimeoutMs, kJiraOverallTimeoutMs);
}

cpr::Response JiraGetLogged(const std::string& url, const cpr::Header& headers, long connectTimeoutMs,
                            long overallTimeoutMs) {
    cpr::Redirect redirect(true, true);
    cpr::Response response = cpr::Get(cpr::Url{url}, headers, redirect, cpr::ConnectTimeout{connectTimeoutMs},
                                      cpr::Timeout{overallTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira, NetworkUsageTracker::kEstimatedGetUploadBytes,
                                           response);
    LogJiraHttpResult("GET", url, response);
    return response;
}

cpr::Response JiraPostLogged(const std::string& url, const cpr::Header& headers, const std::string& body) {
    cpr::Redirect redirect(true, true);
    cpr::Response response = cpr::Post(cpr::Url{url}, headers, cpr::Body{body}, redirect,
                                       cpr::ConnectTimeout{kJiraConnectTimeoutMs}, cpr::Timeout{kJiraOverallTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira, static_cast<std::uint64_t>(body.size()), response);
    LogJiraHttpResult("POST", url, response);
    return response;
}

cpr::Response JiraPutLogged(const std::string& url, const cpr::Header& headers, const std::string& body) {
    cpr::Redirect redirect(true, true);
    cpr::Response response = cpr::Put(cpr::Url{url}, headers, cpr::Body{body}, redirect,
                                      cpr::ConnectTimeout{kJiraConnectTimeoutMs}, cpr::Timeout{kJiraOverallTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira, static_cast<std::uint64_t>(body.size()), response);
    LogJiraHttpResult("PUT", url, response);
    return response;
}

bool IsJiraTransportErrorText(const std::string& error) {
    if (error.empty()) {
        return false;
    }
    const std::string s = ToLowerAsciiCopy(error);

    // Client/config/auth/validation — never treat as transport.
    static const char* kHard[] = {
        "missing jira domain",
        "missing jira",
        "api token",
        "tracker backend is not initialized",
        "http 400",
        "http 401",
        "http 402",
        "http 403",
        "http 404",
        "http 405",
        "http 406",
        "http 409",
        "http 410",
        "http 422",
        "invalid credentials",
        "bad request",
        "unprocessable",
    };
    for (const char* h : kHard) {
        if (s.find(h) != std::string::npos) {
            return false;
        }
    }

    static const char* kTransport[] = {
        "http 0",
        "http 500",
        "http 502",
        "http 503",
        "http 504",
        "timeout",
        "timed out",
        "operation timed out",
        "could not resolve host",
        "couldn't resolve host",
        "name or service not known",
        "failed to connect",
        "connection refused",
        "connection reset",
        "connection aborted",
        "network is unreachable",
        "host unreachable",
        "ssl connect error",
        "couldn't connect to server",
        "eof occurred",
        "offline",
        "network error",
        "resolve host",
        "resolve proxy",
        "connection closed",
        "stream error",
        "certificate verify failed",
        "ssl peer certificate",
        "schannel",
        // Broad connectivity hints (aligned with legacy offline-create detection).
        "network",
        "connection",
    };
    for (const char* t : kTransport) {
        if (s.find(t) != std::string::npos) {
            return true;
        }
    }
    return false;
}
