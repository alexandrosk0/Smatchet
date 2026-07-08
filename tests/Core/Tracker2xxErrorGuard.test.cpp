// Tracker2xxErrorGuard doctest — DR20 (tracker read-path half).
//
// Pure building-block coverage for the "2xx-non-200 / 200-with-errors swallowed
// as Ok()" guard added at the Linear/GitHub read-path call sites (LinearClient
// FetchFieldCatalog, LinearIssueSearch FetchIssuesForKeysViaGraphQl, GitHubClient
// FetchIssueComments). Those sites reach a *failure* branch and then classified
// the status with TrackerErrorFromHttpStatus — which returns Ok() for ANY 2xx,
// silently discarding the detail and misclassifying the failure as success.
//
// The HTTP-fixture end-to-end assertions live with the cpr-linked client tests
// (mirrors tests/Core/TrackerCatalogBuild.test.cpp). This TU pins the pure
// classifier contract the guard depends on, so a future refactor of
// TrackerErrorFromHttpStatus can't quietly reintroduce the swallow.

#include "Tracker/TrackerError.h"

#include <doctest/doctest.h>

TEST_CASE("DR20 — TrackerErrorFromHttpStatus returns Ok() for every 2xx (the swallow this guard fixes)") {
    // Root cause: a genuine failure branch that hands a 2xx status to this helper
    // gets an Ok sentinel back (Kind==None) with the detail discarded.
    for (int status : {200, 201, 202, 204, 299}) {
        const TrackerError e = TrackerErrorFromHttpStatus(status, "boom");
        CHECK(e.IsOk());
        CHECK(e.Kind == TrackerErrorKind::None);
        CHECK(e.Detail.empty());
    }
}

TEST_CASE("DR20 — the guard's replacement carries a non-OK kind and preserves the detail") {
    // What the call sites now emit for a 2xx that reached the failure branch.
    const TrackerError e = TrackerErrorUnknown("GraphQL errors: rate limited", 200);
    CHECK_FALSE(e.IsOk());
    CHECK(e.Kind == TrackerErrorKind::Unknown);
    CHECK(e.Detail == "GraphQL errors: rate limited");
    CHECK(e.HttpStatus == 200);

    const TrackerError e202 = TrackerErrorUnknown("HTTP 202 detail", 202);
    CHECK_FALSE(e202.IsOk());
    CHECK_FALSE(e202.Detail.empty());
    CHECK(e202.HttpStatus == 202);
}

TEST_CASE("DR20 — genuine non-2xx still classifies normally (guard must not shadow it)") {
    // The guard only intercepts the 2xx range; real errors keep their kinds.
    CHECK(TrackerErrorFromHttpStatus(401, "x").Kind == TrackerErrorKind::Auth);
    CHECK(TrackerErrorFromHttpStatus(404, "x").Kind == TrackerErrorKind::NotFound);
    CHECK(TrackerErrorFromHttpStatus(429, "x").Kind == TrackerErrorKind::RateLimited);
    CHECK(TrackerErrorFromHttpStatus(500, "x").Kind == TrackerErrorKind::ServerError);
    CHECK(TrackerErrorFromHttpStatus(0, "x").Kind == TrackerErrorKind::Transport);
}
