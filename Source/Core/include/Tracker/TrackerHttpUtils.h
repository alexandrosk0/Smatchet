#pragma once

#include "ConfigManager.h"
#include "TrackerHttpPure.h"

#include <cpr/cpr.h>

#include <string>

constexpr long kTrackerConnectTimeoutMs = 5000;
constexpr long kTrackerOverallTimeoutMs = 30000;
/** Short timeouts for periodic connectivity probes (avoid blocking state updates on stalled TCP). */
constexpr long kTrackerProbeConnectTimeoutMs = 2000;
constexpr long kTrackerProbeOverallTimeoutMs = 5000;

std::string UrlEncode(const std::string& value);
std::string NormalizeBaseUrl(const std::string& domain);
/**
 * Redact a tracker HTTP response/error body before it is written to a log line
 * (security synthesis #12). Tracker 4xx/5xx bodies have been observed reflecting
 * the request — a 401/403 can echo the `Authorization` header verbatim, and Jira
 * personal-access / GitHub PAT tokens surface in error payloads. The raw body was
 * previously logged through `TruncateForLog` (truncate-only, no key-name / token
 * redaction). This delegates the token-shape stripping to the cpr-free
 * `smatchet::ai::pure::RedactProviderErrorBody` (same Bearer / api_key /
 * Authorization / sk-/ghp_ heuristics) then caps the length — never invent a new
 * redactor. Returns a redacted, length-capped copy safe to log.
 */
std::string RedactHttpBodyForLog(const std::string& body);
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
void LogTrackerHttpResult(const char* clientName, const char* method, const std::string& url,
                          const cpr::Response& response);

// IsTrackerTransportErrorText moved to TrackerHttpPure.h (cpr-free) — still declared via the
// include above, so existing includers of this header keep the decl transitively.
