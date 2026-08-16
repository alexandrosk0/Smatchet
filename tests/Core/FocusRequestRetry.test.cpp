// FocusRequestRetry.test.cpp — bucket-A coverage of the bounded retry policy behind the
// docked-tab focus latch (issue #2058).
//
// The defect: `requestPreferencesFocus` / `requestUserInfoFocus` were cleared on the same
// frame SmatchetUI::selectDockedTab was called. ImGui only creates the ImGuiWindow inside
// Begin(), so on the FIRST-EVER open selectDockedTab found nothing, did nothing, and the
// request was thrown away regardless — the window came up behind its docked sibling and
// stayed there, which is the exact failure the latch exists to prevent.
//
// selectDockedTab now reports whether it resolved a window, and FocusRequestStaysArmed
// decides whether the latch survives the frame. Pure — no ImGui, no UiDrawSession — so the
// edge semantics are pinned here rather than only inside a live frame. The ImGui half (the
// FindWindowByName / DockNode->TabBar walk) has no headless seam and is exercised by
// tests/ui/docked_tab_focus.test.cpp.

#include "Ui/SmatchetFocusRequest.h"

#include <doctest/doctest.h>

using smatchet::ui::FocusRequestStaysArmed;

namespace {
// Mirrors SmatchetUI::kFocusRequestRetryFrames. Kept local so this file links against the
// header alone; the production constant is asserted to be small-and-positive by the callers.
const int kBudget = 4;
} // namespace

TEST_CASE("FocusRequestStaysArmed: a serviced request clears the latch immediately") {
    int retries = 0;
    CHECK_FALSE(FocusRequestStaysArmed(true, retries, kBudget));
    CHECK(retries == 0);
}

TEST_CASE("FocusRequestStaysArmed: an unresolved request holds the latch and counts the frame") {
    // The #2058 frame: the window has not run its first Begin, so there is nothing to focus
    // yet and the request must survive to be retried.
    int retries = 0;
    CHECK(FocusRequestStaysArmed(false, retries, kBudget));
    CHECK(retries == 1);
}

TEST_CASE("FocusRequestStaysArmed: the first-ever-open sequence resolves on the next frame") {
    // Frame 1: Begin has not run, selectDockedTab misses, latch held.
    int retries = 0;
    CHECK(FocusRequestStaysArmed(false, retries, kBudget));
    // Frame 2: the window exists now, the tab is raised, latch cleared and the counter reset
    // so the next focus request starts from a full budget.
    CHECK_FALSE(FocusRequestStaysArmed(true, retries, kBudget));
    CHECK(retries == 0);
}

TEST_CASE("FocusRequestStaysArmed: a name that never resolves gives up after the budget") {
    int retries = 0;
    for (int frame = 0; frame < kBudget; ++frame) {
        CHECK(FocusRequestStaysArmed(false, retries, kBudget));
        CHECK(retries == frame + 1);
    }
    // Budget spent — the latch is released rather than re-issuing SetNextWindowFocus forever
    // and trapping focus on a window the user has moved on from.
    CHECK_FALSE(FocusRequestStaysArmed(false, retries, kBudget));
    CHECK(retries == 0);
}

TEST_CASE("FocusRequestStaysArmed: giving up leaves a full budget for the next request") {
    int retries = 0;
    for (int frame = 0; frame < kBudget; ++frame) {
        CHECK(FocusRequestStaysArmed(false, retries, kBudget));
    }
    CHECK_FALSE(FocusRequestStaysArmed(false, retries, kBudget));
    // A later, genuine request must get the same number of retries the first one did.
    CHECK(FocusRequestStaysArmed(false, retries, kBudget));
    CHECK(retries == 1);
}

TEST_CASE("FocusRequestStaysArmed: a zero budget never holds the latch") {
    // Degenerate guard — a caller that opts out of retries behaves exactly like the old
    // clear-on-the-same-frame code rather than looping.
    int retries = 0;
    CHECK_FALSE(FocusRequestStaysArmed(false, retries, 0));
    CHECK(retries == 0);
    CHECK_FALSE(FocusRequestStaysArmed(false, retries, -1));
    CHECK(retries == 0);
}
