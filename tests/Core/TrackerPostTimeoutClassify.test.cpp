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
