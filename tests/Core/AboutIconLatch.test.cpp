#include <doctest/doctest.h>

#include "Ui/AboutIconLatchPure.h"

// The About-icon failure latch (SmatchetAboutIcon.cpp). The rule under test is that the two
// failure modes are NOT alike: a decode failure is deterministic in the baked bytes and latches,
// while an upload failure depends on live renderer state and must stay retryable. Latching both
// was the defect — one early frame where the renderer had not yet advertised texture support
// bought a glyph-only icon for the rest of the session.

using smatchet::ui::AboutIconAttemptsBlocked;
using smatchet::ui::AboutIconFailureAction;
using smatchet::ui::AboutIconLatch;
using smatchet::ui::IconLoadStep;
using smatchet::ui::OnAboutIconFailure;

TEST_CASE("AboutIconLatch — a fresh latch blocks nothing") {
    AboutIconLatch latch;
    CHECK_FALSE(AboutIconAttemptsBlocked(latch));
}

TEST_CASE("AboutIconLatch — an UPLOAD failure never blocks retries (the regression)") {
    AboutIconLatch latch;

    const AboutIconFailureAction first = OnAboutIconFailure(IconLoadStep::Upload, latch);
    CHECK_FALSE(first.blockFurtherAttempts);
    CHECK(first.shouldLog); // first occurrence is reported
    CHECK_FALSE(AboutIconAttemptsBlocked(latch));

    SUBCASE("repeated upload failures keep retrying and stop logging") {
        for (int i = 0; i < 5; ++i) {
            const AboutIconFailureAction again = OnAboutIconFailure(IconLoadStep::Upload, latch);
            CHECK_FALSE(again.blockFurtherAttempts);
            CHECK_FALSE(again.shouldLog); // one line, not one per frame
        }
        // The whole point: after any number of upload failures the caller may still try again,
        // so the icon appears as soon as the renderer is ready.
        CHECK_FALSE(AboutIconAttemptsBlocked(latch));
    }
}

TEST_CASE("AboutIconLatch — a DECODE failure blocks permanently and logs once") {
    AboutIconLatch latch;

    const AboutIconFailureAction first = OnAboutIconFailure(IconLoadStep::Decode, latch);
    CHECK(first.blockFurtherAttempts);
    CHECK(first.shouldLog);
    CHECK(AboutIconAttemptsBlocked(latch));

    const AboutIconFailureAction second = OnAboutIconFailure(IconLoadStep::Decode, latch);
    CHECK(second.blockFurtherAttempts);
    CHECK_FALSE(second.shouldLog); // already reported
    CHECK(AboutIconAttemptsBlocked(latch));
}

TEST_CASE("AboutIconLatch — NoBytesBaked blocks permanently; nothing can change it") {
    AboutIconLatch latch;
    const AboutIconFailureAction action = OnAboutIconFailure(IconLoadStep::NoBytesBaked, latch);
    CHECK(action.blockFurtherAttempts);
    CHECK(action.shouldLog);
    CHECK(AboutIconAttemptsBlocked(latch));
}

TEST_CASE("AboutIconLatch — an earlier upload failure does not weaken a later decode latch") {
    // Ordering matters: the upload path must not consume the decode path's log-once budget, and a
    // decode failure after retryable upload failures must still latch.
    AboutIconLatch latch;
    OnAboutIconFailure(IconLoadStep::Upload, latch);
    CHECK_FALSE(AboutIconAttemptsBlocked(latch));

    const AboutIconFailureAction decode = OnAboutIconFailure(IconLoadStep::Decode, latch);
    CHECK(decode.blockFurtherAttempts);
    CHECK(decode.shouldLog); // the decode warning is still emitted, despite the earlier upload one
    CHECK(AboutIconAttemptsBlocked(latch));
}

TEST_CASE("AboutIconLatch — a blocked latch stays blocked even if an upload is reported after") {
    AboutIconLatch latch;
    OnAboutIconFailure(IconLoadStep::Decode, latch);
    OnAboutIconFailure(IconLoadStep::Upload, latch);
    CHECK(AboutIconAttemptsBlocked(latch)); // Upload must never clear a permanent latch
}
