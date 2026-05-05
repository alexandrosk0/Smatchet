#pragma once

#include "ConfigManager.h"

#include <cpr/cpr.h>

#include <string>

constexpr long kJiraConnectTimeoutMs = 5000;
constexpr long kJiraOverallTimeoutMs = 30000;
/** Short timeouts for periodic connectivity probes (avoid blocking state updates on stalled TCP). */
constexpr long kJiraProbeConnectTimeoutMs = 2000;
constexpr long kJiraProbeOverallTimeoutMs = 5000;

std::string UrlEncode(const std::string& value);
std::string NormalizeBaseUrl(const std::string& domain);
bool EnsureJiraAuthConfig(const JiraConfig& cfg, std::string& outError);
cpr::Header BuildJiraHeaders(const JiraConfig& cfg, bool includeJsonContentType = false);
std::string BuildJiraBasicAuthHeader(const JiraConfig& cfg);
cpr::Response JiraGetLogged(const std::string& url, const cpr::Header& headers);
cpr::Response JiraGetLogged(const std::string& url, const cpr::Header& headers, long connectTimeoutMs,
                            long overallTimeoutMs);
cpr::Response JiraPostLogged(const std::string& url, const cpr::Header& headers, const std::string& body);
cpr::Response JiraPutLogged(const std::string& url, const cpr::Header& headers, const std::string& body);
void LogTrackerHttpResult(const char* clientName, const char* method, const std::string& url, const cpr::Response& response);

/**
 * True when `error` looks like a connectivity/transport failure (HTTP 0, timeouts, DNS, etc.).
 * False for missing auth/domain, typical HTTP 4xx validation/auth responses, and unknown strings
 * (conservative: callers should not treat unknown errors as offline/transport).
 */
bool IsTrackerTransportErrorText(const std::string& error);
