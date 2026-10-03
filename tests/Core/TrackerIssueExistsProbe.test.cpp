#include <doctest/doctest.h>

#include "Tracker/TrackerHttpPure.h"

// TrackerHttpPure::ClassifyIssueExistsProbe is the one ProbeIssueExists verdict for the Jira, GitHub
// and Plane clients (each used to carry its own copy). The reconcile deletes a cached row only on
// Ok(false), so the table below is what keeps an outage or a throttle from deleting tickets.

using TrackerHttpPure::ClassifyIssueExistsProbe;

TEST_CASE("ClassifyIssueExistsProbe: 200 exists, 404 deleted") {
    const auto exists = ClassifyIssueExistsProbe(200);
    REQUIRE(exists.has_value());
    CHECK(exists.value());

    const auto deleted = ClassifyIssueExistsProbe(404);
    REQUIRE(deleted.has_value());
    CHECK_FALSE(deleted.value());
}

TEST_CASE("ClassifyIssueExistsProbe: any other status is an inconclusive error, never a deletion") {
    // Another 2xx would read as Ok in TrackerErrorFromHttpStatus; it must stay an error.
    const auto noContent = ClassifyIssueExistsProbe(204);
    REQUIRE_FALSE(noContent.has_value());
    CHECK(noContent.error().Kind == TrackerErrorKind::Unknown);
    CHECK(noContent.error().HttpStatus == 204);
    CHECK(noContent.error().Detail == "ProbeIssueExists: unexpected 2xx");

    // No response at all (cpr status 0) stays a retryable Transport error.
    const auto offline = ClassifyIssueExistsProbe(0);
    REQUIRE_FALSE(offline.has_value());
    CHECK(offline.error().Kind == TrackerErrorKind::Transport);
    CHECK(offline.error().IsRetryable());

    CHECK(ClassifyIssueExistsProbe(401).error().Kind == TrackerErrorKind::Auth);
    CHECK(ClassifyIssueExistsProbe(429).error().Kind == TrackerErrorKind::RateLimited);
    CHECK(ClassifyIssueExistsProbe(503).error().Kind == TrackerErrorKind::ServerError);
    CHECK(ClassifyIssueExistsProbe(410).error().Kind == TrackerErrorKind::InvalidRequest);
    CHECK(ClassifyIssueExistsProbe(503).error().Detail == "ProbeIssueExists HTTP error");
}
