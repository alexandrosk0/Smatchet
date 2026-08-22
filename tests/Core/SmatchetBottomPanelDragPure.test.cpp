// Pure-logic coverage for the bottom-panel drag-to-hide / drag-to-reveal gesture
// thresholds (SmatchetBottomPanelDragPure.h). These pin the P4V-style contract the
// impure driver (SmatchetBottomPanelDrag.cpp) feeds live dock geometry into: the
// splitter must be dragged deliberately past the panel's min-height clamp to hide it,
// the reveal grip must travel meaningfully upward to open it, and reveal/restore
// heights stay inside the splitter's own clamp range.

#include "Ui/SmatchetBottomPanelDragPure.h"

#include <doctest/doctest.h>

namespace pure = SmatchetBottomPanelDragPure;

TEST_CASE("HideArmBandPx sits below the splitter's min-height clamp and never degenerates") {
    // Default style (WindowMinSize.y = 32): the band is 24px — inside the bottom 32px
    // the splitter clamp reserves, so the mouse only enters it by dragging past the clamp.
    CHECK(pure::HideArmBandPx(32.0f) == doctest::Approx(24.0f));
    CHECK(pure::HideArmBandPx(32.0f) < 32.0f);
    // A tiny/zero style min still leaves a usable band (floor at 12px).
    CHECK(pure::HideArmBandPx(0.0f) == doctest::Approx(12.0f));
    CHECK(pure::HideArmBandPx(10.0f) == doctest::Approx(12.0f));
    // A scaled-up style scales the band with it.
    CHECK(pure::HideArmBandPx(64.0f) == doctest::Approx(48.0f));
}

TEST_CASE("ShouldArmHide arms only inside the band hugging the panel's bottom edge") {
    const float panelBottom = 1000.0f;
    const float band = 24.0f;
    CHECK(pure::ShouldArmHide(976.0f, panelBottom, band));       // exactly on the band edge
    CHECK(pure::ShouldArmHide(999.0f, panelBottom, band));       // deep in the band
    CHECK(pure::ShouldArmHide(1040.0f, panelBottom, band));      // dragged past the edge (status bar / off-window)
    CHECK_FALSE(pure::ShouldArmHide(975.0f, panelBottom, band)); // just above the band — plain resize
    CHECK_FALSE(pure::ShouldArmHide(700.0f, panelBottom, band)); // mid-panel
}

TEST_CASE("RevealDragCrossedThreshold requires meaningful upward travel") {
    const float start = 900.0f;
    // A click or wiggle never opens the panel.
    CHECK_FALSE(pure::RevealDragCrossedThreshold(start, start));
    CHECK_FALSE(pure::RevealDragCrossedThreshold(start, start - 5.0f));
    // Downward travel never opens it either.
    CHECK_FALSE(pure::RevealDragCrossedThreshold(start, start + 50.0f));
    // At/past the threshold it opens.
    CHECK(pure::RevealDragCrossedThreshold(start, start - pure::kRevealDragThresholdPx));
    CHECK(pure::RevealDragCrossedThreshold(start, start - 200.0f));
}

TEST_CASE("ShouldCollapseOnRevealRelease needs a real open before a release-in-band cancels") {
    const float workBottom = 1000.0f;
    const float band = 24.0f;
    // The open threshold (12px) sits INSIDE the band: a minimal open-drag released
    // immediately (peak ~12-20px) must NOT re-collapse, or the panel flashes open/shut.
    CHECK_FALSE(pure::ShouldCollapseOnRevealRelease(12.0f, 988.0f, workBottom, band));
    CHECK_FALSE(pure::ShouldCollapseOnRevealRelease(40.0f, 985.0f, workBottom, band));
    // A drag that genuinely opened the panel (peak well above the band) and came back
    // down inside the band cancels the reveal.
    CHECK(pure::ShouldCollapseOnRevealRelease(300.0f, 990.0f, workBottom, band));
    // Same peak released ABOVE the band keeps the panel open at the released height.
    CHECK_FALSE(pure::ShouldCollapseOnRevealRelease(300.0f, 700.0f, workBottom, band));
}

TEST_CASE("RevealHeightForMouseY tracks the mouse and clamps to the splitter's own limits") {
    const float workBottom = 1000.0f;
    const float minH = 32.0f;
    const float maxH = 850.0f;
    // Panel top edge tracks the mouse: mouse at 700 -> 300px tall.
    CHECK(pure::RevealHeightForMouseY(700.0f, workBottom, minH, maxH) == doctest::Approx(300.0f));
    // Below the work bottom (drag started, barely moved) -> min height, never negative.
    CHECK(pure::RevealHeightForMouseY(1050.0f, workBottom, minH, maxH) == doctest::Approx(minH));
    // Dragged to the very top -> capped at maxH, the panel can't swallow the viewport.
    CHECK(pure::RevealHeightForMouseY(0.0f, workBottom, minH, maxH) == doctest::Approx(maxH));
}

TEST_CASE("RestoreHeight prefers a sane remembered height and falls back to the default") {
    const float minH = 32.0f;
    const float workH = 1000.0f;
    // Remembered height wins when it is a real (>-min) height.
    CHECK(pure::RestoreHeight(400.0f, minH, workH) == doctest::Approx(400.0f));
    // Never remembered (0) or degenerate (<= min) -> the default reveal height.
    CHECK(pure::RestoreHeight(0.0f, minH, workH) == doctest::Approx(pure::kDefaultRevealHeightPx));
    CHECK(pure::RestoreHeight(20.0f, minH, workH) == doctest::Approx(pure::kDefaultRevealHeightPx));
    // A huge remembered height (the work area shrank since the hide) is capped to the
    // work-share ceiling instead of swallowing the viewport.
    CHECK(pure::RestoreHeight(5000.0f, minH, workH) == doctest::Approx(workH * pure::kMaxRevealWorkShare));
    // A degenerate work area still returns at least the minimum.
    CHECK(pure::RestoreHeight(400.0f, minH, 0.0f) == doctest::Approx(minH));
}
