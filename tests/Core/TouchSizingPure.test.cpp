// Pure-logic coverage for touch-sized hit targets (Ui/TouchSizingPure.h): the Auto-mode detector
// switches on at the first touch and back off only once the mouse is clearly in use again, and the
// style overlay only ever grows hit targets, never overlaps neighbours, and is idempotent.

#include "Ui/TouchSizingPure.h"

#include <doctest/doctest.h>

using namespace SmatchetTouchSizingPure;

namespace {

TouchStyleFields DefaultDesktopStyle() {
    TouchStyleFields f;
    f.itemSpacingX = 8.0f;
    f.itemSpacingY = 6.0f;
    f.touchExtraPaddingX = 0.0f;
    f.touchExtraPaddingY = 0.0f;
    f.scrollbarSize = 14.0f;
    f.grabMinSize = 12.0f;
    f.framePaddingY = 4.0f;
    f.windowBorderHoverPadding = 4.0f;
    f.tabCloseButtonMinWidthUnselected = 0.0f;
    return f;
}

} // namespace

TEST_SUITE("TouchSizingPure") {
    TEST_CASE("detector turns on at the first touch") {
        TouchSizingDetector d;
        CHECK_FALSE(d.Active());
        d.OnTouch(1.0);
        CHECK(d.Active());
    }

    TEST_CASE("mouse use right after a touch does not switch sizing off") {
        TouchSizingDetector d;
        d.OnTouch(1.0);
        d.OnMouseButtonOrWheel(1.5);
        d.OnMouseMove(1.6, 500.0f);
        CHECK(d.Active());
    }

    TEST_CASE("a click or wheel after the dwell switches sizing off") {
        TouchSizingDetector d;
        d.OnTouch(1.0);
        d.OnMouseButtonOrWheel(1.0 + kOffDwellSeconds);
        CHECK_FALSE(d.Active());
    }

    TEST_CASE("small pointer jitter does not switch sizing off, sustained travel does") {
        TouchSizingDetector d;
        d.OnTouch(0.0);
        // Jitter spread out over many seconds never accumulates inside one travel window.
        for (int i = 0; i < 20; ++i) {
            d.OnMouseMove(3.0 + 1.5 * i, 5.0f);
        }
        CHECK(d.Active());
        // 60 px inside one second does.
        for (int i = 0; i < 6; ++i) {
            d.OnMouseMove(40.0 + 0.1 * i, 10.0f);
        }
        CHECK_FALSE(d.Active());
    }

    TEST_CASE("a new touch restarts the dwell") {
        TouchSizingDetector d;
        d.OnTouch(0.0);
        d.OnTouch(5.0);
        d.OnMouseButtonOrWheel(6.0);
        CHECK(d.Active());
        d.OnMouseButtonOrWheel(7.0);
        CHECK_FALSE(d.Active());
    }

    TEST_CASE("overlay enlarges the desktop defaults") {
        const TouchStyleFields out = ComputeTouchTargetStyle(DefaultDesktopStyle(), 1.0f);
        CHECK_EQ(out.touchExtraPaddingX, doctest::Approx(4.0f));
        CHECK_EQ(out.touchExtraPaddingY, doctest::Approx(3.0f));
        CHECK_EQ(out.scrollbarSize, doctest::Approx(kTouchScrollbarSizePx));
        CHECK_EQ(out.grabMinSize, doctest::Approx(kTouchGrabMinSizePx));
        CHECK_EQ(out.framePaddingY, doctest::Approx(kTouchFramePaddingYPx));
        CHECK_EQ(out.windowBorderHoverPadding, doctest::Approx(kTouchWindowBorderHoverPaddingPx));
        CHECK_EQ(out.tabCloseButtonMinWidthUnselected, -1.0f);
        // Item spacing itself (layout) is left alone.
        CHECK_EQ(out.itemSpacingX, 8.0f);
        CHECK_EQ(out.itemSpacingY, 6.0f);
    }

    TEST_CASE("padded hit boxes of neighbours never overlap") {
        TouchStyleFields wide = DefaultDesktopStyle();
        wide.itemSpacingX = 30.0f;
        wide.itemSpacingY = 3.0f;
        const TouchStyleFields out = ComputeTouchTargetStyle(wide, 1.0f);
        CHECK_LE(2.0f * out.touchExtraPaddingY, wide.itemSpacingY);
        CHECK_LE(out.touchExtraPaddingX, kTouchExtraPaddingMaxPx); // capped
    }

    TEST_CASE("overlay is idempotent and never shrinks a roomier style") {
        const TouchStyleFields once = ComputeTouchTargetStyle(DefaultDesktopStyle(), 1.0f);
        const TouchStyleFields twice = ComputeTouchTargetStyle(once, 1.0f);
        CHECK_EQ(twice.touchExtraPaddingX, once.touchExtraPaddingX);
        CHECK_EQ(twice.scrollbarSize, once.scrollbarSize);
        CHECK_EQ(twice.framePaddingY, once.framePaddingY);

        TouchStyleFields roomy = DefaultDesktopStyle();
        roomy.scrollbarSize = 30.0f;
        roomy.framePaddingY = 10.0f;
        const TouchStyleFields out = ComputeTouchTargetStyle(roomy, 1.0f);
        CHECK_EQ(out.scrollbarSize, 30.0f);
        CHECK_EQ(out.framePaddingY, 10.0f);
    }

    TEST_CASE("overlay minimums follow the host density scale") {
        const TouchStyleFields out = ComputeTouchTargetStyle(DefaultDesktopStyle(), 2.0f);
        CHECK_EQ(out.scrollbarSize, doctest::Approx(2.0f * kTouchScrollbarSizePx));
        CHECK_EQ(out.framePaddingY, doctest::Approx(2.0f * kTouchFramePaddingYPx));
    }
}
