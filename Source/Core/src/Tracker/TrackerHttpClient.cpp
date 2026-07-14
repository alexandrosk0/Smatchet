#include "TrackerHttpClient.h"

#include "Logger.h"
#include "TrackerHttpPure.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {

// Server-requested retry delay from the response's `Retry-After` header, in seconds.
// -1 when the header is absent or unparseable (cpr::Header compares case-insensitively).
long RetryAfterSecondsFrom(const cpr::Response& response) {
    const cpr::Header::const_iterator it = response.header.find("Retry-After");
    if (it == response.header.end()) {
        return -1;
    }
    return TrackerHttpPure::ParseRetryAfterSeconds(it->second);
}

} // namespace

TrackerHttpResult ClassifyTrackerResponse(const cpr::Response& response) {
    TrackerHttpResult out;
    out.Response = response;

    const int status = static_cast<int>(response.status_code);

    // Build the detail string preferring the upstream body, then cpr's error message, then a
    // generic "HTTP <code>" so callers always have something to log / show.
    std::string detail = response.text;
    if (detail.empty() && !response.error.message.empty()) {
        detail = response.error.message;
    }
    if (detail.empty()) {
        if (status > 0) {
            detail = "HTTP " + std::to_string(status);
        } else {
            detail = "Unknown network error";
        }
    }

    // A 403 carrying `Retry-After` is a throttle wearing an auth status code — GitHub's
    // secondary rate limits and abuse detection respond exactly this way. Classify it as
    // rate-limited, which is retryable and an offline-queueable transient, instead of a
    // hard auth failure. A plain 403 without the header stays an auth failure.
    if (status == 403 && RetryAfterSecondsFrom(response) >= 0) {
        out.Error = TrackerErrorRateLimited(std::move(detail), status);
        return out;
    }

    out.Error = TrackerErrorFromHttpStatus(status, std::move(detail));
    return out;
}

TrackerReachabilityProbeResult ClassifyReachabilityProbe(const cpr::Response& response) {
    const TrackerHttpResult classified = ClassifyTrackerResponse(response);
    TrackerReachabilityProbeResult out;
    switch (classified.Error.Kind) {
    case TrackerErrorKind::None:
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
        break;
    case TrackerErrorKind::Auth:
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Auth Error)";
        break;
    case TrackerErrorKind::ServerError:
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Server Error)";
        break;
    case TrackerErrorKind::NotFound:
    case TrackerErrorKind::InvalidRequest:
    case TrackerErrorKind::RateLimited:
        // Reachable, but the response indicates a config / payload issue rather than a transport
        // failure — surface the auth-or-config banner so a stale base URL or a rate-limited probe
        // doesn't flip the connectivity banner to "offline".
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status());
        break;
    case TrackerErrorKind::Transport:
    default:
        out.Kind = TrackerReachabilityProbeKind::TransportDown;
        out.Diagnostic = response.error.message.empty() ? classified.Error.Detail : response.error.message;
        break;
    }
    return out;
}

TrackerHttpResult TrackerHttpRequestWithRetry(const std::function<TrackerHttpResult()>& requestFn, int maxAttempts,
                                              const std::function<bool()>& cancelled,
                                              const std::function<bool(const TrackerError&)>& shouldRetry) {
    if (!requestFn) {
        TrackerHttpResult err;
        err.Error = TrackerErrorUnknown("TrackerHttpRequestWithRetry called with empty requestFn");
        return err;
    }
    if (maxAttempts < 1) {
        maxAttempts = 1;
    }

    TrackerHttpResult result;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (cancelled && cancelled()) {
            result.Error = TrackerErrorCancelled("Cancelled before attempt " + std::to_string(attempt));
            return result;
        }

        result = requestFn();
        const bool retryable = shouldRetry ? shouldRetry(result.Error) : result.Error.IsRetryable();
        if (result.IsOk() || !retryable || attempt == maxAttempts) {
            return result;
        }

        // Exponential backoff (250, 500, 1000, … capped at 4000 ms), raised to the
        // server-requested `Retry-After` when the response carried one — itself capped
        // (kTrackerHttpMaxRetryAfterHonorMs) so a hostile value can't pin the worker; the
        // sleep below polls the cancel token throughout, so a longer honored delay stays
        // shutdown-safe.
        const long delayMs = TrackerHttpPure::ComputeTrackerRetryDelayMs(
            attempt, kTrackerHttpBaseRetryDelayMs, kTrackerHttpMaxRetryDelayMs, RetryAfterSecondsFrom(result.Response),
            kTrackerHttpMaxRetryAfterHonorMs);

        LOG_DEBUG("TrackerHttpClient: %s on attempt %d/%d (status %d); retrying after %ld ms",
                  ToString(result.Error.Kind), attempt, maxAttempts, result.Status(), delayMs);

        // Poll cancelled at small intervals so shutdown / supersede can break a long backoff.
        constexpr long kPollIntervalMs = 50;
        long waited = 0;
        while (waited < delayMs) {
            if (cancelled && cancelled()) {
                result.Error =
                    TrackerErrorCancelled("Cancelled during backoff before attempt " + std::to_string(attempt + 1));
                return result;
            }
            const long step = (std::min)(kPollIntervalMs, delayMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            waited += step;
        }
    }

    return result;
}
