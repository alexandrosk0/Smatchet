#pragma once

#include "ConfigManager.h"

#include <cpr/cpr.h>

#include <string>

constexpr long kTrackerConnectTimeoutMs = 5000;
constexpr long kTrackerOverallTimeoutMs = 30000;
/** Short timeouts for periodic connectivity probes (avoid blocking state updates on stalled TCP). */
constexpr long kTrackerProbeConnectTimeoutMs = 2000;
constexpr long kTrackerProbeOverallTimeoutMs = 5000;

std::string UrlEncode(const std::string& value);
std::string NormalizeBaseUrl(const std::string& domain);
bool EnsureTrackerAuthConfig(const TrackerConfig& cfg, std::string& outError);
cpr::Header BuildTrackerHeaders(const TrackerConfig& cfg, bool includeJsonContentType = false);
std::string BuildTrackerBasicAuthHeader(const TrackerConfig& cfg);
/**
 * Redirect policy for every tracker request (security H4 / E2): redirect-following DISABLED
 * so a cross-host 30x can never forward the caller-set raw Basic/Bearer `Authorization`
 * header to an attacker/MITM host (curl's UNRESTRICTED_AUTH default only governs USERPWD,
 * not raw headers). Use this for any direct cpr verb that carries tracker auth and is not
 * already routed through the Tracker*Logged helpers (e.g. multipart attachment upload).
 */
cpr::Redirect MakeTrackerRedirectPolicy();
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers);
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const cpr::Parameters& params);
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               long connectTimeoutMs, long overallTimeoutMs);
cpr::Response TrackerPostLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                const std::string& body);
cpr::Response TrackerPutLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const std::string& body);
cpr::Response TrackerPatchLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                 const std::string& body);
void LogTrackerHttpResult(const char* clientName, const char* method, const std::string& url, const cpr::Response& response);

/**
 * True when `error` looks like a connectivity/transport failure (HTTP 0, timeouts, DNS, etc.).
 * False for missing auth/domain, typical HTTP 4xx validation/auth responses, and unknown strings
 * (conservative: callers should not treat unknown errors as offline/transport).
 */
bool IsTrackerTransportErrorText(const std::string& error);






