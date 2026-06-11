#include <doctest/doctest.h>

#include "TicketFieldEditorDurationPopupPure.h"

using TicketFieldEditorDurationPopupPure::IsPrintableTypedChar;
using TicketFieldEditorDurationPopupPure::ShouldFinalizeOnPopupClose;
using TicketFieldEditorDurationPopupPure::ShouldPullFocusForTypedChar;

// estimate-edit-ux: the duration-suggestions popup steals keyboard focus from its InputText
// (popup focus clears the parent ActiveId). These policies decide (a) when a printable keystroke
// pulls focus back into the input (type-to-edit) and (b) when a popup-close is a genuine
// commit-deactivation vs a click landing back on the input (refocus-for-editing, never a commit —
// the old unconditional popup-close commit queued a spurious empty-value PUT on an empty buffer).

TEST_CASE("IsPrintableTypedChar: printable characters land in the input") {
    CHECK(IsPrintableTypedChar(' '));
    CHECK(IsPrintableTypedChar('2'));
    CHECK(IsPrintableTypedChar('h'));
    CHECK(IsPrintableTypedChar('~'));
    CHECK(IsPrintableTypedChar(0x00E9u)); // é — non-ASCII text is typed text too
}

TEST_CASE("IsPrintableTypedChar: WM_CHAR control codes for navigation/dismiss keys are filtered") {
    CHECK_FALSE(IsPrintableTypedChar('\r')); // Enter
    CHECK_FALSE(IsPrintableTypedChar('\n'));
    CHECK_FALSE(IsPrintableTypedChar(0x1Bu)); // Escape
    CHECK_FALSE(IsPrintableTypedChar('\b'));  // Backspace
    CHECK_FALSE(IsPrintableTypedChar('\t'));  // Tab
    CHECK_FALSE(IsPrintableTypedChar(0x7Fu)); // DEL
}

TEST_CASE("ShouldPullFocusForTypedChar: pulls focus when the keystroke would otherwise drop") {
    // Grid editor open, popup holds focus (no active item), printable char queued -> pull.
    CHECK(ShouldPullFocusForTypedChar(/*typeToEditEnabled=*/true, /*anyItemActive=*/false,
                                      /*focusAlreadyPending=*/false, /*hasPrintableQueuedChar=*/true));
}

TEST_CASE("ShouldPullFocusForTypedChar: never steals from an active item") {
    // Another InputText (e.g. a sibling field) is consuming the queue — do not steal focus.
    CHECK_FALSE(ShouldPullFocusForTypedChar(true, /*anyItemActive=*/true, false, true));
}

TEST_CASE("ShouldPullFocusForTypedChar: disabled outside the grid inline-edit path") {
    // Worklog-dialog callers keep stock focus behaviour.
    CHECK_FALSE(ShouldPullFocusForTypedChar(/*typeToEditEnabled=*/false, false, false, true));
}

TEST_CASE("ShouldPullFocusForTypedChar: no-ops without a printable char or with focus pending") {
    CHECK_FALSE(ShouldPullFocusForTypedChar(true, false, false, /*hasPrintableQueuedChar=*/false));
    CHECK_FALSE(ShouldPullFocusForTypedChar(true, false, /*focusAlreadyPending=*/true, true));
}

TEST_CASE("ShouldFinalizeOnPopupClose: genuine click-away close commits (pre-fix behaviour kept)") {
    CHECK(ShouldFinalizeOnPopupClose(/*popupJustClosed=*/true, /*focusReturnPending=*/false,
                                     /*inputActive=*/false, /*inputHovered=*/false));
}

TEST_CASE("ShouldFinalizeOnPopupClose: click landing back on the input is a refocus, not a commit") {
    // Click re-activated the input naturally (caret placed by the click).
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, false, /*inputActive=*/true, /*inputHovered=*/true));
    // Popup-close ate the click: input only hovered — still a refocus (caller re-arms focus).
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, false, /*inputActive=*/false, /*inputHovered=*/true));
}

TEST_CASE("ShouldFinalizeOnPopupClose: suggestion pick (focus-return pending) never finalizes here") {
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, /*focusReturnPending=*/true, false, false));
}

TEST_CASE("ShouldFinalizeOnPopupClose: nothing closed, nothing finalized") {
    CHECK_FALSE(ShouldFinalizeOnPopupClose(/*popupJustClosed=*/false, false, false, false));
    CHECK_FALSE(ShouldFinalizeOnPopupClose(false, false, true, true));
}
