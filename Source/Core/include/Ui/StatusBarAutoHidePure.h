#pragma once

// Pure, ImGui-free state machine for the status bar auto-hide feature. The bar hides
// when idle and reveals on (a) tracker/connectivity problems and (b) pointer hover over
// the bottom edge. Keeping the logic here decouples visibility decisions from ImGui,
// letting tests pin the contract without a context.

namespace StatusBarAutoHidePure {

// Status bar visibility mode derived from ShowStatusBar + StatusBarAutoHide config flags.
enum class Mode { Always, AutoHide, Hidden };

// Map the two config booleans to the three visibility modes.
inline Mode ModeFromConfig(bool show, bool autoHide) {
    if (!show)
        return Mode::Hidden;
    return autoHide ? Mode::AutoHide : Mode::Always;
}

// Signals that determine whether the status bar needs attention. All are cleared /
// recomputed per frame.
struct Signals {
    bool trackerProblem;   // true for auth/config error, transport down, or service unavailable
    std::size_t queuedOps; // pending creates + pending field edits
    bool savingEdit;       // edit is in-flight to the backend
    int unreadErrors;      // count from SmatchetToastManager

    bool operator==(const Signals& other) const {
        return trackerProblem == other.trackerProblem && queuedOps == other.queuedOps &&
               savingEdit == other.savingEdit && unreadErrors == other.unreadErrors;
    }

    bool operator!=(const Signals& other) const { return !(*this == other); }
};

// Returns true if any signal warrants showing the status bar.
inline bool NeedsAttention(const Signals& signals) {
    return signals.trackerProblem || signals.queuedOps > 0 || signals.savingEdit || signals.unreadErrors > 0;
}

// State machine tracking visibility flashes, hover dwell, and grace period after pointer leaves.
struct State {
    Signals last{};
    bool hasLast = false;
    double flashUntil = 0;   // timestamp when the 3 s flash expires
    double hoverSince = -1;  // timestamp when pointer entered the zone; -1 if not hovering
    double hoverLeftAt = -1; // timestamp when pointer left the zone; -1 if still hovering
    bool visible = false;    // was the bar visible the previous frame?
};

// Flash duration after a signal change.
const double kFlashSeconds = 3.0;

// Height of the band at the bottom edge that reveals the bar when the pointer lingers.
const float kRevealZonePx = 6.0f;

// Dwell time before the pointer reveal engages.
const double kHoverDwellSeconds = 0.25;

// Grace period after the pointer leaves before the bar hides again.
const double kHoverGraceSeconds = 0.4;

// Determine whether the mouse pointer is in the reveal zone at the bottom of the work area.
// When the panel is collapsed, inset the zone by the grip height so it doesn't overlay it.
// Returns true if the pointer is close enough to the bottom edge to trigger auto-reveal.
inline bool PointerInZone(float mouseY, float workBottomY, float insetPx, float barH, bool visible) {
    if (visible) {
        // Bar is showing: zone is the area immediately above where the bar is drawn
        // (from the bar's top edge down to the bottom of the work area).
        return mouseY >= workBottomY - insetPx - barH;
    }
    // Bar is hidden: zone is defined by max(kRevealZonePx, insetPx) pixels from the bottom.
    // When insetPx > 0 (grip present), the zone is inset to not overlap the grip.
    const float threshold = workBottomY - (insetPx > 0 ? insetPx : kRevealZonePx);
    return mouseY <= threshold;
}

// Update the state machine based on the current signals and pointer state.
// Returns true if the bar should be visible this frame.
// - First frame only records the baseline without a flash.
// - A transition to an attention state after the first frame arms a 3 s flash.
// - Pointer reveal needs the dwell time and no mouse button held.
// - The bar stays visible while attention is needed, during the flash, during hover dwell,
//   or within the grace period after pointer leaves.
inline bool Tick(State& state, const Signals& now, double t, bool pointerInZone, bool anyMouseDown) {
    // Detect a transition to an attention state. Only flash after the first frame.
    const bool needsAttentionNow = NeedsAttention(now);
    if (state.hasLast) {  // not the first frame
        const bool needsAttentionBefore = NeedsAttention(state.last);
        if (needsAttentionNow && !needsAttentionBefore) {
            state.flashUntil = t + kFlashSeconds;
        }
    }

    // Update the baseline for next frame.
    if (!state.hasLast) {
        state.hasLast = true;
    }
    state.last = now;

    // Pointer reveal logic: dwell timer needs no buttons held, and leaving the zone
    // starts the grace period.
    if (pointerInZone && !anyMouseDown) {
        if (state.hoverSince < 0) {
            state.hoverSince = t; // dwell timer starts now
        }
        state.hoverLeftAt = -1; // still in zone; cancel grace
    } else {
        if (state.hoverSince >= 0 && state.hoverLeftAt < 0) {
            state.hoverLeftAt = t; // just left the zone
        }
        state.hoverSince = -1; // no longer in zone; reset dwell timer
    }

    // Dwell is met when the pointer has rested long enough.
    const bool hoverDwellMet = state.hoverSince >= 0 && (t - state.hoverSince >= kHoverDwellSeconds);

    // Grace period is active when the pointer left recently.
    const bool inGracePeriod = state.hoverLeftAt >= 0 && (t - state.hoverLeftAt < kHoverGraceSeconds);

    // Decide visibility.
    state.visible = NeedsAttention(now) || t < state.flashUntil || hoverDwellMet || inGracePeriod;
    return state.visible;
}

} // namespace StatusBarAutoHidePure
