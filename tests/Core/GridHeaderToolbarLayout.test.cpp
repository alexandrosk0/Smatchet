#include "SmatchetGridHeaderUi_detail.h"

#include <doctest/doctest.h>

using SmatchetGridHeader::detail::AccumulateRightWidth;
using SmatchetGridHeader::detail::ChipFadeFraction;
using SmatchetGridHeader::detail::RightClusterWidths;

TEST_CASE("ChipFadeFraction clamps to the unit interval") {
    CHECK(ChipFadeFraction(0.0f, 5.0f) == doctest::Approx(0.0f));
    CHECK(ChipFadeFraction(2.5f, 5.0f) == doctest::Approx(0.5f));
    CHECK(ChipFadeFraction(5.0f, 5.0f) == doctest::Approx(1.0f));
    // Over-run clamps high; negative elapsed clamps low.
    CHECK(ChipFadeFraction(7.0f, 5.0f) == doctest::Approx(1.0f));
    CHECK(ChipFadeFraction(-1.0f, 5.0f) == doctest::Approx(0.0f));
}

TEST_CASE("ChipFadeFraction treats a non-positive duration as fully faded") {
    CHECK(ChipFadeFraction(1.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(ChipFadeFraction(1.0f, -3.0f) == doctest::Approx(1.0f));
}

TEST_CASE("AccumulateRightWidth: empty cluster in read-only mode is zero") {
    RightClusterWidths w;
    w.betweenSpacing = 8.0f;
    CHECK(AccumulateRightWidth(w, /*readOnlyMode=*/true) == doctest::Approx(0.0f));
}

TEST_CASE("AccumulateRightWidth: lone New Issue button, no spacer") {
    RightClusterWidths w;
    w.newIssueBtnW = 60.0f;
    w.betweenSpacing = 8.0f;
    // First (and only) visible element: no leading spacer.
    CHECK(AccumulateRightWidth(w, /*readOnlyMode=*/false) == doctest::Approx(60.0f));
}

TEST_CASE("AccumulateRightWidth: tracker + button inserts one spacer between them") {
    RightClusterWidths w;
    w.showTrackerChip = true;
    w.trackerW = 100.0f;
    w.newIssueBtnW = 60.0f;
    w.betweenSpacing = 8.0f;
    // 100 (tracker) + 8 (spacer) + 60 (button).
    CHECK(AccumulateRightWidth(w, /*readOnlyMode=*/false) == doctest::Approx(168.0f));
}

TEST_CASE("AccumulateRightWidth: all chips plus button in read-only omits the button") {
    RightClusterWidths w;
    w.showTrackerChip = true;
    w.trackerW = 100.0f;
    w.showReadOnlyChip = true;
    w.readOnlyW = 50.0f;
    w.showMcpChip = true;
    w.mcpW = 70.0f;
    w.newIssueBtnW = 60.0f;
    w.betweenSpacing = 8.0f;
    // 100 + 8 + 50 + 8 + 70 = 236 (button suppressed in read-only).
    CHECK(AccumulateRightWidth(w, /*readOnlyMode=*/true) == doctest::Approx(236.0f));
    // 236 + 8 + 60 = 304 once the button is shown.
    CHECK(AccumulateRightWidth(w, /*readOnlyMode=*/false) == doctest::Approx(304.0f));
}
