#include <doctest/doctest.h>

#include "OfflineQueueReplayPolicy.h"

using OfflineQueueReplayPolicy::kMaxReplayAttempts;
using OfflineQueueReplayPolicy::ShouldArchive;

TEST_CASE("OfflineQueueReplayPolicy::kMaxReplayAttempts is the shipped cap") {
    // Hard-coded sentinel — bumping this requires touching the offline-replay
    // user docs + LocalCacheManager aliases + the dead-letter audit-trail
    // expectations. Test exists so an unintentional bump fails CI.
    CHECK(kMaxReplayAttempts == 5);
}

TEST_CASE("ShouldArchive — pre-attempt gate (currentAttempts vs cap)") {
    // Below cap → keep retrying.
    CHECK_FALSE(ShouldArchive(0));
    CHECK_FALSE(ShouldArchive(1));
    CHECK_FALSE(ShouldArchive(4));

    // At cap → archive (next replay would exceed the budget).
    CHECK(ShouldArchive(5));

    // Past cap (race / repair / migration left a row stuck) → still archive.
    CHECK(ShouldArchive(6));
    CHECK(ShouldArchive(100));
}

TEST_CASE("ShouldArchive — post-failure gate (nextAttempts = currentAttempts + 1)") {
    // Simulates the OfflineQueueService.cpp post-failure call site:
    //   const int nextAttempts = row.Attempts + 1;
    //   if (ShouldArchive(nextAttempts)) { ... archive ... }
    // Verify the boundary lands on the 5th attempt's failure.
    for (int currentAttempts = 0; currentAttempts < 10; ++currentAttempts) {
        const int nextAttempts = currentAttempts + 1;
        const bool shouldArchive = ShouldArchive(nextAttempts);
        CAPTURE(currentAttempts);
        CAPTURE(nextAttempts);
        CHECK(shouldArchive == (nextAttempts >= 5));
    }
}

TEST_CASE("ShouldArchive — explicit maxAttempts override") {
    // Tighter cap (e.g. integration-test override).
    CHECK_FALSE(ShouldArchive(0, 1));
    CHECK(ShouldArchive(1, 1));

    // Looser cap.
    CHECK_FALSE(ShouldArchive(9, 10));
    CHECK(ShouldArchive(10, 10));
    CHECK(ShouldArchive(11, 10));

    // Pathological zero cap — always archive.
    CHECK(ShouldArchive(0, 0));
    CHECK(ShouldArchive(1, 0));
}

TEST_CASE("ShouldArchive — handles negative attempts gracefully") {
    // Defensive: a corrupted row with Attempts < 0 should still be allowed
    // through (not archived) so the queue service can fix it up.
    CHECK_FALSE(ShouldArchive(-1));
    CHECK_FALSE(ShouldArchive(-100));
}
