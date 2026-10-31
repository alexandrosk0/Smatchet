#pragma once

#include <algorithm>
#include <cmath>

// Touch-sized hit targets for the desktop hosts. Two pure pieces, both ImGui-free so they are unit
// tested (tests/Core/TouchSizingPure.test.cpp):
//   - TouchSizingDetector decides, in Auto mode, whether the UI should currently be touch-sized:
//     on at the first touch, off again only once the mouse is clearly back in use. The off rule is
//     deliberately sticky so a stray synthetic mouse move after a tap cannot flip the layout.
//   - ComputeTouchTargetStyle enlarges the style fields that set how big a hit target is. Every
//     field takes the max of its current value and a touch minimum, so applying it twice is the
//     same as applying it once and a style that is already roomy (Comfortable density, Mobile
//     mode) is left alone. Font size is never touched.
// The ImGui glue copies ImGuiStyle fields into TouchStyleFields and back (SmatchetTheme.cpp).
namespace SmatchetTouchSizingPure {

// Mouse must be in use at least this long after the last touch before sizing can switch off.
constexpr double kOffDwellSeconds = 2.0;
// ...and then either a click / wheel, or this much pointer travel inside kMouseTravelWindowSeconds.
constexpr float kMouseTravelOffPx = 48.0f;
constexpr double kMouseTravelWindowSeconds = 1.0;

class TouchSizingDetector {
  public:
    void OnTouch(double now) {
        active_ = true;
        lastTouch_ = now;
        travel_ = 0.0f;
    }

    void OnMouseButtonOrWheel(double now) {
        if (active_ && now - lastTouch_ >= kOffDwellSeconds) {
            active_ = false;
        }
    }

    void OnMouseMove(double now, float distancePx) {
        if (!active_ || now - lastTouch_ < kOffDwellSeconds) {
            travel_ = 0.0f;
            return;
        }
        if (travel_ <= 0.0f || now - travelStart_ > kMouseTravelWindowSeconds) {
            travelStart_ = now;
            travel_ = 0.0f;
        }
        travel_ += std::fabs(distancePx);
        if (travel_ >= kMouseTravelOffPx) {
            active_ = false;
            travel_ = 0.0f;
        }
    }

    bool Active() const { return active_; }

  private:
    bool active_ = false;
    double lastTouch_ = 0.0;
    double travelStart_ = 0.0;
    float travel_ = 0.0f;
};

// Touch minimums, logical pixels (multiplied by the host density scale).
constexpr float kTouchExtraPaddingMaxPx = 6.0f;
constexpr float kTouchScrollbarSizePx = 20.0f;
constexpr float kTouchGrabMinSizePx = 20.0f;
constexpr float kTouchFramePaddingYPx = 6.0f;
constexpr float kTouchWindowBorderHoverPaddingPx = 8.0f;

// The subset of ImGuiStyle that sets hit-target size.
struct TouchStyleFields {
    float itemSpacingX = 0.0f;
    float itemSpacingY = 0.0f;
    float touchExtraPaddingX = 0.0f;
    float touchExtraPaddingY = 0.0f;
    float scrollbarSize = 0.0f;
    float grabMinSize = 0.0f;
    float framePaddingY = 0.0f;
    float windowBorderHoverPadding = 0.0f;
    float tabCloseButtonMinWidthUnselected = 0.0f;
};

inline TouchStyleFields ComputeTouchTargetStyle(const TouchStyleFields& in, float scale) {
    const float s = (scale > 0.0f) ? scale : 1.0f;
    TouchStyleFields out = in;
    // Half the item spacing on each side, so the padded hit boxes of two neighbours meet but never
    // overlap (ImGui does not arbitrate overlapping TouchExtraPadding).
    const float padCap = kTouchExtraPaddingMaxPx * s;
    const float padX = std::min(padCap, std::max(0.0f, std::floor(in.itemSpacingX * 0.5f)));
    const float padY = std::min(padCap, std::max(0.0f, std::floor(in.itemSpacingY * 0.5f)));
    out.touchExtraPaddingX = std::max(in.touchExtraPaddingX, padX);
    out.touchExtraPaddingY = std::max(in.touchExtraPaddingY, padY);
    out.scrollbarSize = std::max(in.scrollbarSize, kTouchScrollbarSizePx * s);
    out.grabMinSize = std::max(in.grabMinSize, kTouchGrabMinSizePx * s);
    out.framePaddingY = std::max(in.framePaddingY, std::round(kTouchFramePaddingYPx * s));
    out.windowBorderHoverPadding = std::max(in.windowBorderHoverPadding, kTouchWindowBorderHoverPaddingPx * s);
    // A finger cannot hover a tab to reveal its close button, so always show it.
    out.tabCloseButtonMinWidthUnselected = -1.0f;
    return out;
}

} // namespace SmatchetTouchSizingPure
