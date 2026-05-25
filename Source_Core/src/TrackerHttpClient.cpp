#include "TrackerHttpClient.h"

#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <thread>

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

    out.Error = TrackerErrorFromHttpStatus(status, std::move(detail));
    return out;
}

TrackerHttpResult TrackerHttpRequestWithRetry(const std::function<TrackerHttpResult()>& requestFn, int maxAttempts,
                                              const std::function<bool()>& cancelled) {
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
        if (result.IsOk() || !result.Error.IsRetryable() || attempt == maxAttempts) {
            return result;
        }

        // Exponential backoff: 250, 500, 1000, 2000, 4000 ms (capped). Cheap clamp via std::min.
        long delayMs = kTrackerHttpBaseRetryDelayMs;
        for (int i = 1; i < attempt; ++i) {
            delayMs *= 2;
            if (delayMs >= kTrackerHttpMaxRetryDelayMs) {
                delayMs = kTrackerHttpMaxRetryDelayMs;
                break;
            }
        }

        LOG_DEBUG("TrackerHttpClient: %s on attempt %d/%d (status %d); retrying after %ld ms",
                  ToString(result.Error.Kind), attempt, maxAttempts, result.Status(), delayMs);

        // Poll cancelled at small intervals so shutdown / supersede can break a long backoff.
        constexpr long kPollIntervalMs = 50;
        long waited = 0;
        while (waited < delayMs) {
            if (cancelled && cancelled()) {
                result.Error = TrackerErrorCancelled("Cancelled during backoff before attempt " +
                                                    std::to_string(attempt + 1));
                return result;
            }
            const long step = (std::min)(kPollIntervalMs, delayMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            waited += step;
        }
    }

    return result;
}
