// Finding DR16 — pure coverage of the POST retry decision (TrackerShouldRetryPost in
// TrackerError.h). A non-idempotent POST may be re-sent only after a pre-send transport failure
// that provably never reached the server; a post-send operation timeout (the server may already
// have committed the create/comment) must be single-attempt or it double-fires. cpr collapses both
// connect- and read-phase timeouts to status 0 -> TrackerErrorKind::Transport, so the operation-
// timeout flag — not the kind alone — decides retryability. No socket, no cpr: the helper is a pure
// boolean decision driven directly.

#include "TrackerError.h"

#include <doctest/doctest.h>

TEST_CASE("TrackerShouldRetryPost retries a pre-send transport failure") {
    // Genuine pre-send transport failure (DNS / refused connect), NOT an operation timeout.
    CHECK(TrackerShouldRetryPost(TrackerErrorKind::Transport, /*operationTimeout=*/false));
}

TEST_CASE("TrackerShouldRetryPost does NOT retry a post-send operation timeout") {
    // Status-0 Transport that came from OPERATION_TIMEDOUT: the server may already have committed,
    // so re-sending would double-create / double-comment.
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::Transport, /*operationTimeout=*/true));
}

TEST_CASE("TrackerShouldRetryPost never retries a landed non-transport result") {
    // A request that landed then returned 429 / 5xx / auth / etc. is never re-sent regardless of
    // the timeout flag — only Transport is ever eligible.
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::RateLimited, false));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::ServerError, false));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::ServerError, true));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::Auth, false));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::None, false));
}
