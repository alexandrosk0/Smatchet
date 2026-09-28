// Finding DR16 — pure coverage of the POST retry decision (TrackerShouldRetryPost in TrackerError.h). A
// non-idempotent POST is re-sent only when the request provably never left this machine (RequestNotSent:
// a DNS / proxy / connect failure); a timeout or lost response may follow an applied create / comment, so
// re-sending it would double-fire. Which cpr error codes prove "not sent" is covered in TrackerHttpRetry.

#include "TrackerError.h"

#include <doctest/doctest.h>

TEST_CASE("TrackerShouldRetryPost requires positive evidence of a pre-send failure") {
    CHECK(TrackerShouldRetryPost(TrackerErrorKind::Transport, true));
    // Operation timeouts, lost responses, and unspecified transport errors are ambiguous.
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::Transport, false));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::RateLimited, true));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::ServerError, true));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::Auth, true));
    CHECK_FALSE(TrackerShouldRetryPost(TrackerErrorKind::None, true));
}

TEST_CASE("ProvablyNotApplied — never sent, or refused unprocessed (429)") {
    TrackerError refused = TrackerErrorTransport("connection refused");
    CHECK_FALSE(refused.ProvablyNotApplied()); // a transport failure alone may follow an applied request
    refused.RequestNotSent = true;
    CHECK(refused.ProvablyNotApplied());
    CHECK(TrackerErrorRateLimited("slow down").ProvablyNotApplied());
    CHECK_FALSE(TrackerErrorServer("HTTP 503", 503).ProvablyNotApplied());
    CHECK_FALSE(TrackerErrorTransport("Operation timed out").ProvablyNotApplied());
}
