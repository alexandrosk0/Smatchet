// DR28 — pure backoff decision behind SmatchetFieldIconRender's per-frame icon fetch/resolve
// negative cache. A failed icon fetch records a failure timestamp; ShouldAttemptIconResolve gates
// whether a later frame re-attempts the (multi-second) HTTP GET or short-circuits within backoff.
// This exercises the extracted pure helper so the decision is testable without the cpr/ImGui rig.

#include "Ui/SmatchetFieldIconRender.h"

#include <doctest/doctest.h>

using SmatchetFieldIconRender::ShouldAttemptIconResolve;

TEST_CASE("DR28: no recorded failure always attempts") {
    // No negative entry -> always resolve, regardless of the timestamps.
    CHECK(ShouldAttemptIconResolve(false, 0, 0, 300000));
    CHECK(ShouldAttemptIconResolve(false, 999999, 0, 300000));
}

TEST_CASE("DR28: negative entry within backoff is suppressed") {
    const long long backoffMs = 300000; // 5 minutes
    const long long failedAt = 1000;
    // 1s after failure with a 5-minute backoff -> still backing off, skip the re-fetch.
    CHECK_FALSE(ShouldAttemptIconResolve(true, failedAt, failedAt + 1000, backoffMs));
    // Right at the boundary minus one ms -> still suppressed.
    CHECK_FALSE(ShouldAttemptIconResolve(true, failedAt, failedAt + backoffMs - 1, backoffMs));
}

TEST_CASE("DR28: negative entry retries once backoff has elapsed") {
    const long long backoffMs = 300000;
    const long long failedAt = 1000;
    // Exactly at the backoff boundary -> allow a fresh attempt.
    CHECK(ShouldAttemptIconResolve(true, failedAt, failedAt + backoffMs, backoffMs));
    // Well past the window -> allow.
    CHECK(ShouldAttemptIconResolve(true, failedAt, failedAt + backoffMs * 4, backoffMs));
}

TEST_CASE("DR28: non-positive backoff disables suppression") {
    // A zero/negative backoff window degrades to always-attempt (never trap a cell permanently).
    CHECK(ShouldAttemptIconResolve(true, 1000, 1000, 0));
    CHECK(ShouldAttemptIconResolve(true, 1000, 1000, -5));
}
