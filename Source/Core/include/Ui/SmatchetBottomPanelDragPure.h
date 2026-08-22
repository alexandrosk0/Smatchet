#pragma once

// Pure, ImGui-free decision helpers for the bottom-panel drag-to-hide / drag-to-reveal
// gesture (P4V-style pane collapse). The impure driver in SmatchetBottomPanelDrag.cpp
// feeds live dock geometry in; keeping the thresholds and decisions here lets
// tests/Core pin the gesture contract without an ImGui context.

namespace SmatchetBottomPanelDragPure {

// Minimum upward travel from the reveal grip before the panel opens, so a plain click
// or a 1-2 px wiggle on the grip never pops the panel open.
const float kRevealDragThresholdPx = 12.0f;

// Fallback panel height for a reveal with no remembered height (fresh session).
const float kDefaultRevealHeightPx = 280.0f;

// Largest share of the work area a drag-revealed / restored panel may take.
const float kMaxRevealWorkShare = 0.85f;

inline float ClampPx(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Height of the band hugging the panel's bottom edge that arms drag-to-hide. The dock
// splitter clamps the panel at the style's minimum window height, so the band must sit
// BELOW that clamp: the mouse only enters it by dragging deliberately past the minimum.
inline float HideArmBandPx(float windowMinSizeY) {
    const float band = windowMinSizeY * 0.75f;
    return band < 12.0f ? 12.0f : band;
}

// Splitter drag: arm the hide when the mouse has been dragged into the band hugging the
// panel's bottom edge - i.e. well past the min-height clamp, mirroring P4V's
// drag-the-border-down-to-collapse.
inline bool ShouldArmHide(float mouseY, float panelBottomY, float bandPx) { return mouseY >= panelBottomY - bandPx; }

// Reveal grip: open only once the drag travelled meaningfully upward.
inline bool RevealDragCrossedThreshold(float dragStartY, float mouseY) {
    return dragStartY - mouseY >= kRevealDragThresholdPx;
}

// Cancel-a-reveal: releasing the reveal drag back inside the hide band collapses the
// panel again ONLY when the drag genuinely opened it first (peak height well above the
// band). The open threshold (kRevealDragThresholdPx) sits INSIDE the band, so without
// the peak requirement a minimal open-drag would flash the panel open and instantly
// re-collapse it on release.
inline bool ShouldCollapseOnRevealRelease(float peakHeightPx, float mouseY, float workBottomY, float bandPx) {
    return peakHeightPx > bandPx * 2.0f && ShouldArmHide(mouseY, workBottomY, bandPx);
}

// Panel height a reveal drag at mouseY should produce (the panel's top edge tracks the
// mouse), clamped to the same limits the splitter would enforce.
inline float RevealHeightForMouseY(float mouseY, float workBottomY, float minHeightPx, float maxHeightPx) {
    return ClampPx(workBottomY - mouseY, minHeightPx, maxHeightPx);
}

// Height for a non-drag reveal (menu / command / fresh session): the remembered height
// when sane, else the default - clamped so a huge remembered value (work area shrank
// since the hide) can never swallow the whole viewport.
inline float RestoreHeight(float rememberedPx, float minHeightPx, float workHeightPx) {
    const float maxH = workHeightPx * kMaxRevealWorkShare;
    const float base = rememberedPx > minHeightPx ? rememberedPx : kDefaultRevealHeightPx;
    return ClampPx(base, minHeightPx, maxH > minHeightPx ? maxH : minHeightPx);
}

} // namespace SmatchetBottomPanelDragPure
