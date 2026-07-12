#pragma once

#include "ConfigManager.h"
#include "TrackerHttpPure.h"

#include <cpr/cpr.h>

#include <functional>
#include <string>

constexpr long kTrackerConnectTimeoutMs = 5000;
constexpr long kTrackerOverallTimeoutMs = 30000;
/** Short timeouts for periodic connectivity probes (avoid blocking state updates on stalled TCP). */
constexpr long kTrackerProbeConnectTimeoutMs = 2000;
constexpr long kTrackerProbeOverallTimeoutMs = 5000;

std::string UrlEncode(const std::string& value);
std::string NormalizeBaseUrl(const std::string& domain);
// RedactHttpBodyForLog moved to TrackerHttpPure.h (cpr-free) so cpr-free targets
// (TSan rig, ConnectivityMonitorService) can link it — still visible via the include above.
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
/**
 * TLS trust options for every tracker request (WS2 / Issue #1068): reads the process-global
 * CA-bundle path (set by the host at boot via TrackerHttpPure::SetCaBundlePath — empty on
 * desktop) and turns it into cpr SSL options. Empty path -> default-constructed SslOptions
 * (peer/host verification stays ON; libcurl uses its system store — desktop behaviour). Non-empty
 * -> an explicit CURLOPT_CAINFO cafile (the Android private-dir cacert.pem). Never disables
 * verification. Use this alongside MakeTrackerRedirectPolicy for any direct cpr verb not routed
 * through the Tracker*Logged helpers (e.g. multipart attachment upload) so those calls get the
 * same trust anchor as the rest of the tracker traffic.
 */
cpr::SslOptions MakeTrackerSslOptions();
// `cancelled` (optional): polled by the retry wrapper before each attempt and after each backoff,
// so a sync worker aborting mid-fetch is observed during the retry/backoff window (not only between
// page GETs). When null, behaves exactly as before (no cancellation polling inside the retry loop).
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const std::function<bool()>& cancelled = nullptr);
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const cpr::Parameters& params, const std::function<bool()>& cancelled = nullptr);
cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               long connectTimeoutMs, long overallTimeoutMs);
// PUT/PATCH/POST mutation helpers: `cancelled` (optional) is forwarded to the retry wrapper exactly
// like the GET overloads above, so an in-flight tracker mutation aborts promptly when an automation
// worker is asked to shut down (#1529 cancel plumbing extended to the mutation path). When null,
// behaves exactly as before (no cancellation polling inside the retry loop).
cpr::Response TrackerPostLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                const std::string& body, const std::function<bool()>& cancelled = nullptr);
cpr::Response TrackerPutLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const std::string& body, const std::function<bool()>& cancelled = nullptr);
cpr::Response TrackerPatchLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                 const std::string& body, const std::function<bool()>& cancelled = nullptr);
void LogTrackerHttpResult(const char* clientName, const char* method, const std::string& url,
                          const cpr::Response& response);

// Transport classification travels as the structured TrackerError kind; there is deliberately no
// text-based transport classifier declared here.
