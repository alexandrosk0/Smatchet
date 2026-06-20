// ShouldPollForChanges — bucket-A coverage for the ticket-change-monitor poll gate
// (Source/Core/include/PaneSyncKickPolicy.h, ticket-change-monitor plan § Approach #1).
// Pure header-only truth-table; no AppController link, mirroring BackgroundWorkerReap.test.cpp.
// Each gate must independently veto a poll so a disabled feature / unreachable backend /
// unfocused window / LRU-evicted pane / in-flight sync / not-yet-due interval never probes.

#include <doctest/doctest.h>

#include <chrono>

#include "PaneSyncKickPolicy.h"

using smatchet::ShouldPollForChanges;

namespace {
// A poll is due when `now` has reached the scheduled next-poll time.
const std::chrono::steady_clock::time_point kNow = std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
const std::chrono::steady_clock::time_point kDue = std::chrono::steady_clock::time_point(std::chrono::seconds(900));
const std::chrono::steady_clock::time_point kNotDue =
    std::chrono::steady_clock::time_point(std::chrono::seconds(1100));
} // namespace

TEST_CASE("All gates open and interval elapsed -> poll") {
    CHECK(ShouldPollForChanges(kNow, /*monitorEnabled=*/true, /*backendReachable=*/true,
                               /*windowFocused=*/true, /*paneRecentlyVisible=*/true,
                               /*syncActive=*/false, kDue) == true);
}

TEST_CASE("Each blocking condition independently vetoes the poll") {
    // Feature disabled.
    CHECK(ShouldPollForChanges(kNow, false, true, true, true, false, kDue) == false);
    // Backend unreachable.
    CHECK(ShouldPollForChanges(kNow, true, false, true, true, false, kDue) == false);
    // Window not focused (no point polling for a window the user is not looking at).
    CHECK(ShouldPollForChanges(kNow, true, true, false, true, false, kDue) == false);
    // Pane LRU-evicted / not recently visible -> empty baseline, must be skipped.
    CHECK(ShouldPollForChanges(kNow, true, true, true, false, false, kDue) == false);
    // A streaming sync is already in flight for the pane.
    CHECK(ShouldPollForChanges(kNow, true, true, true, true, true, kDue) == false);
    // Interval has not elapsed yet (next poll is in the future).
    CHECK(ShouldPollForChanges(kNow, true, true, true, true, false, kNotDue) == false);
}

TEST_CASE("now exactly equal to nextChangePollAt is due (>=, not >)") {
    CHECK(ShouldPollForChanges(kNow, true, true, true, true, false, kNow) == true);
}
