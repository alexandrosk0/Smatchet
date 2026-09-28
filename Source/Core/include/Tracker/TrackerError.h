#pragma once

// TrackerError — small value type for tracker / HTTP errors. Introduced per BACKLOG_CODE_REVIEW.md
// §2.3 / §6.2 / §7 item 15 to replace the ~80+ `bool foo(..., std::string& outError)` signatures
// scattered across `ITrackerBackend`, `JiraClient`, `PlaneClient`, `LocalCacheManager`,
// `IssueCreatePipeline`, `TextMerge`, etc.
// Phase 1 (this PR): introduce the type + helpers. No signature migrated yet. The intent is
// that new APIs use TrackerError; existing APIs keep their `bool + string` shape until
// migrated in follow-up PRs (one subsystem at a time, mechanical).
// Companion macro `TRACKER_ERR_*` factories sit alongside the type so callers can construct
// errors at the call site without naming the enum twice.

#include <cstdint>
#include <string>
#include <utility>

/// Categorical classification of a tracker error. Drives caller decisions about retry,
/// connectivity-banner state, and user-facing messaging. Order matters only for serialization.
enum class TrackerErrorKind : std::uint8_t {
    None = 0,       ///< Sentinel "no error". `IsOk()` returns true.
    Transport,      ///< Network reach failure (DNS, connect, TLS, timeout). Drives the
                    ///< connectivity-down banner; auto-retried by the connectivity probe.
    Auth,           ///< 401 / 403 / token-rotation-mid-flight. Surfaced as "auth or config
                    ///< error"; never auto-retried (would burn a real credential).
    RateLimited,    ///< 429 or vendor-specific quota-exceeded. Retryable with backoff.
    NotFound,       ///< 404 against an issue / project / field. Not auto-retried.
    InvalidRequest, ///< 4xx-other (400, 422). Indicates a payload / config bug, not a
                    ///< transient. Surfaced verbatim to the user.
    ServerError,    ///< 5xx. Retryable with backoff.
    Parse,          ///< Response status was 2xx but the body couldn't be parsed
                    ///< (malformed JSON, missing required key). Not retryable.
    Cancelled,      ///< Caller (e.g. shutdown, supersede) requested abort.
    Unknown,        ///< Catch-all for cases not yet classified. Maps to a generic toast.
};

/// Value type carrying enough information for both diagnostics and decision-making at the
/// call site. Construct via the factory functions below — they keep the call-site noise low
/// and make it easy to grep for error-construction sites.
struct TrackerError {
    TrackerErrorKind Kind = TrackerErrorKind::None;
    /// Human-readable, displayable detail. Often the upstream error body or a hand-written
    /// message. Safe to log; safe to show via toast.
    std::string Detail;
    /// HTTP status if applicable (0 for non-HTTP errors like Parse / Cancelled). Useful for
    /// logging and for the connectivity classifier.
    int HttpStatus = 0;
    /// True only when the request provably never left this machine: a DNS, proxy-resolution or
    /// connect failure, or a precondition that failed before the send. An operation timeout or a
    /// lost response keeps the default (false): the tracker may already have applied the request.
    bool RequestNotSent = false;

    bool IsOk() const noexcept { return Kind == TrackerErrorKind::None; }
    bool IsTransport() const noexcept { return Kind == TrackerErrorKind::Transport; }
    bool IsRetryable() const noexcept {
        return Kind == TrackerErrorKind::Transport || Kind == TrackerErrorKind::RateLimited ||
               Kind == TrackerErrorKind::ServerError;
    }
    /// True when the tracker certainly did not apply the failed request: it was never sent, or the
    /// tracker refused it unprocessed (429). Such a write can be sent again without risking a
    /// duplicate; any other failure of a non-idempotent write (a create, a comment) may have landed.
    bool ProvablyNotApplied() const noexcept { return RequestNotSent || Kind == TrackerErrorKind::RateLimited; }

    static TrackerError Ok() { return TrackerError{}; }
};

/// Factory helpers. Keep call sites readable: `return TrackerError::Transport("connect timeout");`.
inline TrackerError MakeTrackerError(TrackerErrorKind kind, std::string detail, int httpStatus = 0) {
    TrackerError e;
    e.Kind = kind;
    e.Detail = std::move(detail);
    e.HttpStatus = httpStatus;
    return e;
}
inline TrackerError TrackerErrorTransport(std::string detail, int status = 0) {
    return MakeTrackerError(TrackerErrorKind::Transport, std::move(detail), status);
}
inline TrackerError TrackerErrorAuth(std::string detail, int status = 0) {
    return MakeTrackerError(TrackerErrorKind::Auth, std::move(detail), status);
}
inline TrackerError TrackerErrorRateLimited(std::string detail, int status = 429) {
    return MakeTrackerError(TrackerErrorKind::RateLimited, std::move(detail), status);
}
inline TrackerError TrackerErrorNotFound(std::string detail, int status = 404) {
    return MakeTrackerError(TrackerErrorKind::NotFound, std::move(detail), status);
}
inline TrackerError TrackerErrorInvalidRequest(std::string detail, int status = 0) {
    return MakeTrackerError(TrackerErrorKind::InvalidRequest, std::move(detail), status);
}
inline TrackerError TrackerErrorServer(std::string detail, int status = 0) {
    return MakeTrackerError(TrackerErrorKind::ServerError, std::move(detail), status);
}
inline TrackerError TrackerErrorParse(std::string detail) {
    return MakeTrackerError(TrackerErrorKind::Parse, std::move(detail), 0);
}
inline TrackerError TrackerErrorCancelled(std::string detail = "Cancelled") {
    return MakeTrackerError(TrackerErrorKind::Cancelled, std::move(detail), 0);
}
inline TrackerError TrackerErrorUnknown(std::string detail, int status = 0) {
    return MakeTrackerError(TrackerErrorKind::Unknown, std::move(detail), status);
}

/// Classify an HTTP status code into the appropriate TrackerErrorKind. Used by the HTTP-layer
/// shim that wraps cpr responses — saves every call site from re-deriving the same mapping.
/// Status 0 / negative = transport failure (cpr convention).
inline TrackerError TrackerErrorFromHttpStatus(int status, std::string detail) {
    if (status >= 200 && status < 300) {
        return TrackerError::Ok();
    }
    if (status <= 0) {
        return TrackerErrorTransport(std::move(detail), status);
    }
    if (status == 401 || status == 403) {
        return TrackerErrorAuth(std::move(detail), status);
    }
    if (status == 404) {
        return TrackerErrorNotFound(std::move(detail), status);
    }
    if (status == 429) {
        return TrackerErrorRateLimited(std::move(detail), status);
    }
    if (status >= 500 && status < 600) {
        return TrackerErrorServer(std::move(detail), status);
    }
    if (status >= 400 && status < 500) {
        return TrackerErrorInvalidRequest(std::move(detail), status);
    }
    return TrackerErrorUnknown(std::move(detail), status);
}

/// Classify an HTTP status that reached a FAILURE branch (the caller's success check rejected the
/// response). A 2xx-other (201/204/206) still lands here for endpoints whose only success code is
/// 200/204; guard it before TrackerErrorFromHttpStatus, which would map any 2xx to Ok() and drop the
/// failure detail (a false Ok on a return-false path). Keeping the guard in one place holds retry
/// semantics identical across tracker clients (Jira mutation branches + Plane project resolve — #1785).
inline TrackerError ClassifyRejectedHttpStatus(long statusCode, const std::string& detail) {
    const int status = static_cast<int>(statusCode);
    if (status >= 200 && status < 300) {
        return TrackerErrorUnknown(detail, status);
    }
    return TrackerErrorFromHttpStatus(status, detail);
}

/// Retry a non-idempotent POST only with positive evidence that no request was sent.
/// A transport kind alone cannot distinguish DNS failure from a lost response.
inline bool TrackerShouldRetryPost(TrackerErrorKind kind, bool requestNotSent) noexcept {
    return kind == TrackerErrorKind::Transport && requestNotSent;
}

/// Convert a kind to a stable short string for logging. Not user-facing.
inline const char* ToString(TrackerErrorKind k) noexcept {
    switch (k) {
    case TrackerErrorKind::None:
        return "ok";
    case TrackerErrorKind::Transport:
        return "transport";
    case TrackerErrorKind::Auth:
        return "auth";
    case TrackerErrorKind::RateLimited:
        return "rate_limited";
    case TrackerErrorKind::NotFound:
        return "not_found";
    case TrackerErrorKind::InvalidRequest:
        return "invalid_request";
    case TrackerErrorKind::ServerError:
        return "server_error";
    case TrackerErrorKind::Parse:
        return "parse";
    case TrackerErrorKind::Cancelled:
        return "cancelled";
    case TrackerErrorKind::Unknown:
        return "unknown";
    }
    return "unknown";
}
