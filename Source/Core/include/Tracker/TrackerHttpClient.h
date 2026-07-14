#pragma once

// TrackerHttpClient — central HTTP request helper that returns TrackerError-classified results
// and supports retry on transient failures (Transport / RateLimited / ServerError) with
// exponential backoff. Built on top of the existing TrackerXxxLogged helpers in
// TrackerHttpUtils so all calls continue to go through NetworkUsageTracker + body-trace logging.
// Migration plan (BACKLOG_CODE_REVIEW.md §2.3 design proposal #1 / §7 item 15):
//   - Phase 2A (this PR): introduce the helper + value type. Migrate PlaneClient::ProbeReachability
//     as the first call site (also fixes the §2.1 P1: raw cpr::Get bypassing logging + 4xx-
//     misclassified-as-TransportDown bugs).
//   - Phase 2B/2C: incrementally migrate JiraClient / PlaneClient mutation sites and
//     JiraIssueSearch so retry on 429/5xx becomes uniform across the codebase.

#include "ITrackerConnectivity.h" // TrackerReachabilityProbeResult / Kind
#include "TrackerError.h"

#include <cpr/cpr.h>

#include <functional>
#include <string>

constexpr int kTrackerHttpDefaultMaxAttempts = 3;
constexpr long kTrackerHttpBaseRetryDelayMs = 250;
constexpr long kTrackerHttpMaxRetryDelayMs = 4000;
/// Ceiling on honouring a server-sent `Retry-After` between attempts. GitHub's secondary
/// rate limits routinely ask for ~60 s — waiting that long on a sync worker is worse than
/// failing over to the offline queue, so the honored delay is capped here; the backoff
/// sleep polls the cancel token throughout either way.
constexpr long kTrackerHttpMaxRetryAfterHonorMs = 30000;

/// Paired result: typed classification + the raw cpr::Response so callers can still parse the
/// body. `Response.text` is always populated (empty on transport failure). On success,
/// `Error.IsOk()` is true and the caller reads `Response.text`.
struct TrackerHttpResult {
    TrackerError Error;
    cpr::Response Response;

    bool IsOk() const noexcept { return Error.IsOk(); }
    int Status() const noexcept { return static_cast<int>(Response.status_code); }
};

/// Classify a cpr::Response into TrackerError. Maps HTTP status via TrackerErrorFromHttpStatus
/// (so 401/403 = Auth, 404 = NotFound, 429 = RateLimited, 5xx = ServerError, other 4xx =
/// InvalidRequest, status<=0 = Transport). Exception: a 403 carrying a `Retry-After` header is
/// a throttle, not an auth failure (GitHub secondary rate limits / abuse detection respond
/// this way), so it classifies RateLimited — retryable, and an offline-queueable transient for
/// the field-edit chain instead of a hard credential error. The detail string is the upstream
/// body if non-empty, otherwise the cpr error message, otherwise a generic "HTTP <code>" string.
TrackerHttpResult ClassifyTrackerResponse(const cpr::Response& response);

/// Classify a reachability-probe response into a TrackerReachabilityProbeResult, shared by every
/// backend's `ProbeReachability` so the HTTP-status → probe-kind matrix lives in one place:
/// 2xx → AuthenticatedReachable, 401/403 → ReachableAuthOrConfigError, 404/429/other-4xx →
/// ReachableAuthOrConfigError (the host answered — don't flip the banner to offline), 5xx →
/// ServiceUnavailable, transport (status ≤ 0) → TransportDown.
TrackerReachabilityProbeResult ClassifyReachabilityProbe(const cpr::Response& response);

/// Retry loop wrapping a request function. Invokes `requestFn` up to `maxAttempts` times,
/// backing off `kTrackerHttpBaseRetryDelayMs * 2^(attempt-1)` (capped at
/// `kTrackerHttpMaxRetryDelayMs`) between attempts when the result should be retried.
/// `cancelled` (optional) is polled before each attempt and after each backoff; when it returns
/// true, the loop exits with a `Cancelled` TrackerError on whatever the latest response was.
/// `shouldRetry` (optional) decides retryability per error: when null, the default is
/// `TrackerError::IsRetryable()` (Transport / RateLimited / ServerError). Callers wiring a
/// non-idempotent verb (POST) pass a Transport-only predicate so a landed-then-5xx mutation is
/// never re-sent (would double-fire); idempotent verbs keep the default.
TrackerHttpResult TrackerHttpRequestWithRetry(const std::function<TrackerHttpResult()>& requestFn,
                                              int maxAttempts = kTrackerHttpDefaultMaxAttempts,
                                              const std::function<bool()>& cancelled = nullptr,
                                              const std::function<bool(const TrackerError&)>& shouldRetry = nullptr);
