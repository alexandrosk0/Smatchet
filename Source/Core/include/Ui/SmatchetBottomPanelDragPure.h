#pragma once

// Pure, ImGui-free decision helpers for the edge-panel drag-to-hide / drag-to-reveal
// gesture (P4V-style pane collapse). Written in bottom-panel terms (Y, height) but
// axis-neutral: the bottom panel and the right-hand side columns all anchor at their far edge
// (bottom / right), so the driver feeds X values and widths through the same helpers.
// The impure driver in SmatchetBottomPanelDrag.cpp feeds live dock geometry in; keeping
// the thresholds and decisions here lets tests/Core pin the gesture contract without an
// ImGui context.

namespace SmatchetBottomPanelDragPure {

// Height of the reveal grip at the bottom of the work area when the panel is collapsed.
const float kRevealGripHeightPx = 7.0f;

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

// Two hidden side panels may park their reveal grips at the same x. The grip draws unless
// another grip within tolPx belongs to a more recently collapsed panel (LIFO: the panel
// the user hid last is the one they expect to get back first).
inline bool ShouldDrawRevealGrip(float myEdge, unsigned mySeq, float otherEdge, unsigned otherSeq, float tolPx) {
    const float delta = myEdge > otherEdge ? myEdge - otherEdge : otherEdge - myEdge;
    return !(delta <= tolPx && otherSeq > mySeq);
}

// Side-column reveal strip: the press only becomes a reveal once the drag has travelled
// leftward past the open threshold AND clearly more horizontally than vertically, so a
// vertical scrollbar drag or a plain click on the strip is never hijacked.
inline bool RevealDragCommits(float dragStartX, float mouseX, float crossTravelPx) {
    const float dx = dragStartX - mouseX;
    const float absCross = crossTravelPx < 0.0f ? -crossTravelPx : crossTravelPx;
    return dx >= kRevealDragThresholdPx && dx > absCross * 1.5f;
}

// Side-column reveal strip: should the active item keep its press instead of letting a
// reveal drag steal it? Yield to dock splitters / tab bars, text inputs being edited, a
// drag-and-drop or window move in flight, the horizontal scrollbar, a table column drag
// (resize handle or header reorder) and the bottom reveal grip - all of them are horizontal-drag (or must-not-steal)
// interactions. Residual, accepted: sliders / DragFloat that reach the 7px strip are stolen
// once the drag commits.
inline bool ShouldYieldToActiveItem(bool inDockHost, bool textInput, bool dragDropActive, bool horizontalScrollbar,
                                    bool windowMoving, bool tableColumnDrag, bool bottomGrip) {
    return inDockHost || textInput || dragDropActive || horizontalScrollbar || windowMoving || tableColumnDrag ||
           bottomGrip;
}

// The strip may arm / highlight / show a tooltip only when the mouse is over dockspace
// content itself - not through a modal, a floating window above it, a window drag or a
// drag-and-drop.
inline bool SideStripReachable(bool modalOpen, bool windowMoving, bool overDockContent, bool dragDropActive) {
    return !modalOpen && !windowMoving && overDockContent && !dragDropActive;
}

} // namespace SmatchetBottomPanelDragPure
