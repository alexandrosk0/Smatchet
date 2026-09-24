#pragma once

#include <cstddef>

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

// Whether the pointer is in the reveal zone at the bottom of the work area. While the bar
// is hidden the zone is the bottom band - at least kRevealZonePx, or the bottom-panel reveal
// grip's height (insetPx) when the grip is present, so hovering the grip reveals the bar just
// above it. While the bar is visible the zone grows to cover the bar itself (drawn at
// workBottomY - insetPx - barH), so moving onto the bar keeps it open.
inline bool PointerInZone(float mouseY, float workBottomY, float insetPx, float barH, bool visible) {
    if (visible) {
        return mouseY >= workBottomY - insetPx - barH;
    }
    const float bandPx = insetPx > kRevealZonePx ? insetPx : kRevealZonePx;
    return mouseY >= workBottomY - bandPx;
}

// Advance the state machine one frame; returns whether the bar is visible this frame.
// - Any change in signals after the first frame arms a kFlashSeconds flash (the first frame
//   only records the baseline), so a recovery such as offline -> online is still shown.
// - A pointer reveal needs kHoverDwellSeconds in the zone. A held mouse button blocks only
//   STARTING the dwell (so a drag along the bottom edge never pops the bar over a scrollbar
//   or splitter); it never hides a bar that is already visible.
// - After the pointer leaves a visible bar, it stays for kHoverGraceSeconds. Leaving before
//   the dwell completed arms no grace, so passing over the edge never flickers the bar.
inline bool Tick(State& state, const Signals& now, double t, bool pointerInZone, bool anyMouseDown) {
    if (state.hasLast && now != state.last) {
        state.flashUntil = t + kFlashSeconds;
    }
    state.hasLast = true;
    state.last = now;

    if (pointerInZone) {
        if (state.hoverSince < 0 && (!anyMouseDown || state.visible)) {
            // Already visible (attention/flash/grace): the dwell counts as met immediately.
            state.hoverSince = state.visible ? t - kHoverDwellSeconds : t;
        }
        state.hoverLeftAt = -1;
    } else {
        if (state.hoverSince >= 0 && state.visible) {
            state.hoverLeftAt = t;
        }
        state.hoverSince = -1;
    }

    const bool hoverDwellMet = state.hoverSince >= 0 && t - state.hoverSince >= kHoverDwellSeconds;
    const bool inGracePeriod = state.hoverLeftAt >= 0 && t - state.hoverLeftAt < kHoverGraceSeconds;
    state.visible = NeedsAttention(now) || t < state.flashUntil || hoverDwellMet || inGracePeriod;
    return state.visible;
}

} // namespace StatusBarAutoHidePure
