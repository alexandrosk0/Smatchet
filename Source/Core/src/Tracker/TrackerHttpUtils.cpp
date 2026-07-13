#include "TrackerHttpUtils.h"

#include "AiErrorRedact.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpPure.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

constexpr std::size_t kMaxTrackerHttpBodyLogBytes = 65536;
constexpr const char* kTrackerUserAgent = "Smatchet/1.0 Jira-Client";

// Redact URL query for logging: keeps scheme://host/path, drops ?query and #fragment.
std::string RedactUrlForLog(const std::string& url) {
    const size_t q = url.find_first_of("?#");
    if (q == std::string::npos) {
        return url;
    }
    return url.substr(0, q) + "?[redacted]";
}

void LogTrackerHttpResult(const char* clientName, const char* method, const std::string& url,
                          const cpr::Response& response) {
    const int status = static_cast<int>(response.status_code);
    if (status >= 400) {
        LOG_DEBUG("%s: %s %s -> HTTP %d (%zu bytes)", clientName, method, RedactUrlForLog(url).c_str(), status,
                  response.text.size());
    } else if (Logger::Instance().ShouldLog(LogLevel::Trace)) {
        LOG_TRACE("%s: %s %s -> HTTP %d (%zu bytes)", clientName, method, RedactUrlForLog(url).c_str(), status,
                  response.text.size());
    }
    if (!Logger::Instance().GetLogTrackerHttpBodies()) {
        return;
    }
    if (!Logger::Instance().ShouldLog(LogLevel::Trace)) {
        return;
    }
    // Redact reflected tokens before logging the body (security synthesis #12): a
    // tracker 401/403 body can echo the Authorization header / a PAT verbatim.
    std::string body = RedactHttpBodyForLog(response.text);
    std::string suffix;
    if (body.size() > kMaxTrackerHttpBodyLogBytes) {
        body.resize(kMaxTrackerHttpBodyLogBytes);
        suffix = "\n[truncated…]";
    }
    Logger::Instance().Log(LogLevel::Trace, std::string(clientName) + ": " + method + " response body (" +
                                                RedactUrlForLog(url) + "):\n" + body + suffix);
}

// Simple URL-encoder that is C++14 friendly.
std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    (void)escaped.fill('0');
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
    if (base.compare(0, 7, "http://") != 0 && base.compare(0, 8, "https://") != 0) {
        base = "https://" + base;
    }
    // Cleartext-credential hardening (security audit LOW). The tracker Basic-auth header is
    // attached to every request; a cleartext `http://` base to a NON-loopback host would put
    // those credentials on the wire in the clear. Upgrade such a base to `https://` and warn.
    // Loopback `http://` (local dev / a loopback dev tracker) is left as-is.
    if (TrackerHttpPure::ShouldUpgradeCleartextBase(base)) {
        base = "https://" + base.substr(7);
        LOG_WARN("NormalizeBaseUrl: upgraded cleartext http:// tracker base to https:// (credentials must "
                 "not travel in the clear to a non-loopback host); fix the configured domain to https://.");
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}
bool EnsureTrackerAuthConfig(const TrackerConfig& cfg, std::string& outError) {
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing tracker domain or API token.";
        LOG_WARN("TrackerHttpUtils: %s (domain_set=%d token_set=%d)", outError.c_str(), cfg.Domain.empty() ? 0 : 1,
                 cfg.ApiToken.empty() ? 0 : 1);
        return false;
    }
    return true;
}

cpr::Header BuildTrackerHeaders(const TrackerConfig& cfg, bool includeJsonContentType) {
    cpr::Header headers{{"Accept", "application/json"},
                        {"Authorization", BuildTrackerBasicAuthHeader(cfg)},
                        {"User-Agent", kTrackerUserAgent}};
    if (includeJsonContentType) {
        headers["Content-Type"] = "application/json";
    }
    return headers;
}

std::string BuildTrackerBasicAuthHeader(const TrackerConfig& cfg) {
    return "Basic " + Base64Encode(cfg.Email + ":" + cfg.ApiToken);
}

// TLS trust seam (WS2 / Issue #1068). Reads the process-global CA-bundle path (set by the
// host at boot via TrackerHttpPure::SetCaBundlePath — empty on desktop) and turns it into
// cpr SSL options applied to every tracker verb. Empty path -> default-constructed
// SslOptions: peer/host verification stays ON (cpr defaults) and CURLOPT_CAINFO is left
// untouched, so libcurl uses its compile-time / system-store default (desktop behaviour
// unchanged). Non-empty -> an explicit CURLOPT_CAINFO cafile (the Android private-dir
// cacert.pem). This seam never disables verification.
cpr::SslOptions MakeTrackerSslOptions() {
    const TrackerHttpPure::SslConfig ssl = TrackerHttpPure::ResolveSslConfig(TrackerHttpPure::GetCaBundlePath());
    if (!ssl.attach) {
        return cpr::SslOptions{};
    }
    return cpr::Ssl(cpr::ssl::CaInfo{std::string(ssl.caInfoPath)});
}

// Redirect policy for every tracker verb (security: H4 / E2). The tracker `Authorization`
// header is a caller-set raw header, NOT libcurl `CURLOPT_USERPWD`, so curl's
// `CURLOPT_UNRESTRICTED_AUTH=0` default does NOT strip it on a cross-host 30x — a redirect
// from the configured tracker host to an attacker/MITM host would forward the API token.
// cpr (1.9.2) exposes no same-host-only redirect knob, so we disable redirect-following
// entirely: the Jira/Plane/GitHub REST verbs we issue (search / mutation / transitions /
// users / meta / projects / activities) respond directly with 2xx/4xx and never depend on a
// 30x. This mirrors the MCP attachment proxy's `cpr::Redirect(false, false)`
// (Source/Plugins/Mcp/McpPlugin.cpp). A 30x now surfaces as a non-2xx status the callers
// already handle, rather than a silent credential-forwarding follow.
cpr::Redirect MakeTrackerRedirectPolicy() { return cpr::Redirect(/*follow=*/false, /*cont_send_cred=*/false); }

// One GET attempt: cpr::Get + NetworkUsageTracker::Record + LogTrackerHttpResult. Shared by all
// three TrackerGetLogged overloads so the retry-wrapping (or, for the probe overload, deliberate
// single-shot) lives in one place. Redirects are disabled via MakeTrackerRedirectPolicy (security
// H4/E2). Timeouts arrive as std::int32_t — the long->int32 cast happens at each call site (cpr's
// ConnectTimeout/Timeout take int32; a braced-init {long} narrows and is ill-formed under clang on
// LP64). `params` is optional (nullptr = no query parameters).
static cpr::Response ExecuteTrackerGet(const char* clientName, const std::string& url, const cpr::Header& headers,
                                       const cpr::Parameters* params, std::int32_t connectMs, std::int32_t overallMs) {
    cpr::Redirect redirect = MakeTrackerRedirectPolicy();
    cpr::Response response;
    if (params != nullptr) {
        response = cpr::Get(cpr::Url{url}, headers, *params, redirect, cpr::ConnectTimeout{connectMs},
                            cpr::Timeout{overallMs}, MakeTrackerSslOptions());
    } else {
        response = cpr::Get(cpr::Url{url}, headers, redirect, cpr::ConnectTimeout{connectMs}, cpr::Timeout{overallMs},
                            MakeTrackerSslOptions());
    }
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker, NetworkUsageTracker::kEstimatedGetUploadBytes,
                                           response);
    LogTrackerHttpResult(clientName, "GET", url, response);
    return response;
}

cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const std::function<bool()>& cancelled) {
    // Idempotent GET — retry on Transport / 429 / 5xx (wrapper's default IsRetryable predicate).
    // `cancelled` is forwarded so an aborting sync worker is honoured during the retry/backoff
    // window, not only between page GETs (default nullptr = no in-loop cancellation polling).
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            return ClassifyTrackerResponse(ExecuteTrackerGet(clientName, url, headers, nullptr,
                                                             static_cast<std::int32_t>(kTrackerConnectTimeoutMs),
                                                             static_cast<std::int32_t>(kTrackerOverallTimeoutMs)));
        },
        kTrackerHttpDefaultMaxAttempts, cancelled);
    return std::move(result.Response);
}

cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const cpr::Parameters& params, const std::function<bool()>& cancelled) {
    // Idempotent GET (paged) — same retry policy as the no-params overload. `cancelled` is forwarded
    // so a paged search aborting mid-fetch is honoured during the retry/backoff window too.
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            return ClassifyTrackerResponse(ExecuteTrackerGet(clientName, url, headers, &params,
                                                             static_cast<std::int32_t>(kTrackerConnectTimeoutMs),
                                                             static_cast<std::int32_t>(kTrackerOverallTimeoutMs)));
        },
        kTrackerHttpDefaultMaxAttempts, cancelled);
    return std::move(result.Response);
}

cpr::Response TrackerGetLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               long connectTimeoutMs, long overallTimeoutMs) {
    // Single attempt — NO retry. This custom-timeout overload backs periodic reachability probes
    // (2s/5s budget, kTrackerProbe*TimeoutMs). Retrying would multiply the probe's worst-case
    // latency and defeat its fast-fail contract, so it stays single-shot by design. ExecuteTrackerGet
    // already applies MakeTrackerRedirectPolicy() + MakeTrackerSslOptions() (the host-pin / TLS
    // hardening develop added inline), so routing through it keeps that security hardening centralized.
    return ExecuteTrackerGet(clientName, url, headers, nullptr, static_cast<std::int32_t>(connectTimeoutMs),
                             static_cast<std::int32_t>(overallTimeoutMs));
}

cpr::Response TrackerPostLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                const std::string& body, const std::function<bool()>& cancelled) {
    // POST is non-idempotent: a request that LANDED (server applied it) then returned 429/5xx must
    // NEVER be re-sent — that would double-create / double-comment. So the predicate retries only on
    // Transport errors, where the request provably never reached the server (DNS / connect / pre-send
    // failure). 429/5xx after a successful send are returned to the caller untouched.
    //
    // Finding DR16: a POST-SEND timeout (the server may have committed, only the response was lost)
    // also surfaces as status 0 -> Transport, so retrying it double-fires. cpr reports it as
    // OPERATION_TIMEDOUT; we capture the last attempt's error code and treat any operation timeout as
    // non-retryable via TrackerShouldRetryPost, leaving genuine pre-send transport failures retryable.
    cpr::ErrorCode lastErrorCode = cpr::ErrorCode::OK;
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            cpr::Redirect redirect = MakeTrackerRedirectPolicy();
            cpr::Response response = cpr::Post(cpr::Url{url}, headers, cpr::Body{body}, redirect,
                                               cpr::ConnectTimeout{kTrackerConnectTimeoutMs},
                                               cpr::Timeout{kTrackerOverallTimeoutMs}, MakeTrackerSslOptions());
            lastErrorCode = response.error.code;
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker, static_cast<std::uint64_t>(body.size()),
                                                   response);
            LogTrackerHttpResult(clientName, "POST", url, response);
            return ClassifyTrackerResponse(response);
        },
        kTrackerHttpDefaultMaxAttempts, cancelled,
        [&lastErrorCode](const TrackerError& e) {
            return TrackerShouldRetryPost(e.Kind, lastErrorCode == cpr::ErrorCode::OPERATION_TIMEDOUT);
        });
    return std::move(result.Response);
}

cpr::Response TrackerPutLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                               const std::string& body, const std::function<bool()>& cancelled) {
    // PUT is idempotent — safe to retry on Transport / 429 / 5xx (wrapper's default predicate).
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            cpr::Redirect redirect = MakeTrackerRedirectPolicy();
            cpr::Response response = cpr::Put(cpr::Url{url}, headers, cpr::Body{body}, redirect,
                                              cpr::ConnectTimeout{kTrackerConnectTimeoutMs},
                                              cpr::Timeout{kTrackerOverallTimeoutMs}, MakeTrackerSslOptions());
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker, static_cast<std::uint64_t>(body.size()),
                                                   response);
            LogTrackerHttpResult(clientName, "PUT", url, response);
            return ClassifyTrackerResponse(response);
        },
        kTrackerHttpDefaultMaxAttempts, cancelled);
    return std::move(result.Response);
}

// IsTrackerTransportErrorText (formerly here, then TrackerHttpPure.cpp) was deleted in N12
// slice 3 — transport-ness travels as the structured TrackerError kind.

cpr::Response TrackerDeleteLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                  const std::function<bool()>& cancelled) {
    // DELETE on tracker resources is idempotent (deleting an already-deleted element
    // re-reports the same terminal state) — safe to retry like PUT/PATCH.
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            cpr::Redirect redirect = MakeTrackerRedirectPolicy();
            cpr::Response response =
                cpr::Delete(cpr::Url{url}, headers, redirect, cpr::ConnectTimeout{kTrackerConnectTimeoutMs},
                            cpr::Timeout{kTrackerOverallTimeoutMs}, MakeTrackerSslOptions());
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker,
                                                   NetworkUsageTracker::kEstimatedGetUploadBytes, response);
            LogTrackerHttpResult(clientName, "DELETE", url, response);
            return ClassifyTrackerResponse(response);
        },
        kTrackerHttpDefaultMaxAttempts, cancelled);
    return std::move(result.Response);
}

cpr::Response TrackerPatchLogged(const char* clientName, const std::string& url, const cpr::Header& headers,
                                 const std::string& body, const std::function<bool()>& cancelled) {
    // PATCH on tracker fields is idempotent (set-to-value, not delta) — safe to retry like PUT.
    TrackerHttpResult result = TrackerHttpRequestWithRetry(
        [&]() {
            cpr::Redirect redirect = MakeTrackerRedirectPolicy();
            cpr::Response response = cpr::Patch(cpr::Url{url}, headers, cpr::Body{body}, redirect,
                                                cpr::ConnectTimeout{kTrackerConnectTimeoutMs},
                                                cpr::Timeout{kTrackerOverallTimeoutMs}, MakeTrackerSslOptions());
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker, static_cast<std::uint64_t>(body.size()),
                                                   response);
            LogTrackerHttpResult(clientName, "PATCH", url, response);
            return ClassifyTrackerResponse(response);
        },
        kTrackerHttpDefaultMaxAttempts, cancelled);
    return std::move(result.Response);
}
