#pragma once

// Bounded retry policy for a one-frame "raise this window" latch (issue #2058).
//
// The latch — `UiDrawSession::requestPreferencesFocus` and its User Info twin — used to be
// cleared on the same frame it was read, on the assumption that SmatchetUI::selectDockedTab
// had already done its job. It has not, on the one frame that matters most: ImGui only
// creates the ImGuiWindow inside Begin, so on the FIRST-EVER open of a process
// FindWindowByName returns null, selectDockedTab early-returns, and the request is dropped
// anyway — the window comes up behind whichever docked sibling was created first and stays
// there.
//
// The fix is to clear the latch on the frame the request is actually SERVICED, and to hold
// it otherwise. Holding needs a bound: a window name that never resolves — renamed, never
// opened, a locale whose title transform missed — must not re-issue SetNextWindowFocus
// every frame forever and trap focus on a window the user has moved on from. One retry is
// normally enough, since the window exists from the frame after its first Begin, so a small
// cap costs nothing in the good case and turns a stuck request into a no-op in the bad one.
//
// Kept ImGui-free and free-standing so the edge semantics are bucket-A testable
// (tests/Core/FocusRequestRetry.test.cpp) rather than reachable only through a live frame.

namespace smatchet {
namespace ui {

/// Decide whether a focus latch stays armed for another frame.
/// `resolved` is what the focus attempt reported: true when the request was serviced (the
/// docked tab was selected, or the window exists and SetNextWindowFocus alone suffices),
/// false when the window does not exist yet. `retries` is the caller's per-latch counter,
/// updated in place. `maxRetries` bounds how many consecutive unresolved frames the latch
/// survives.
/// Returns true when the caller must NOT clear its latch this frame. Both terminal outcomes
/// — serviced, or the retry budget exhausted — return false and reset `retries` to 0, so the
/// counter is always clean for the next request on the same latch.
inline bool FocusRequestStaysArmed(bool resolved, int& retries, int maxRetries) {
    if (resolved) {
        retries = 0;
        return false;
    }
    if (retries >= maxRetries) {
        // Budget spent: give up rather than hold focus hostage to a name that never resolves.
        retries = 0;
        return false;
    }
    ++retries;
    return true;
}

} // namespace ui
} // namespace smatchet
