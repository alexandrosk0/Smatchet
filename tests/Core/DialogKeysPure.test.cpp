// The pure decisions of the dialog contract (Ui/DialogKeysPure.h; input-ux-w2-dialog-contract): the caret
// return after a dialog or the palette closes (CaretReturnTracker, StepCaretReturn, RequestCaretReturn;
// audit #24 and #89), Esc and the caret request (ResolveEscape, ResolveCaretRequest,
// DismissedPopupComesBack), the confirm default and Enter (ResolveConfirmDefault, ResolveConfirmEnter) and the
// shortcut mute (GlobalShortcutAllowedUnderModal).

#include "Ui/DialogKeysPure.h"

#include <doctest/doctest.h>

using smatchet::dialogkeys::CaretRequest;
using smatchet::dialogkeys::CaretReturnTracker;
using smatchet::dialogkeys::DismissedPopupComesBack;
using smatchet::dialogkeys::EscapeAction;
using smatchet::dialogkeys::RequestCaretReturn;
using smatchet::dialogkeys::ResolveCaretRequest;
using smatchet::dialogkeys::ResolveEscape;
using smatchet::dialogkeys::StepCaretReturn;

namespace {

constexpr unsigned int kField = 0x11u;
constexpr unsigned int kWindow = 0x22u;
constexpr unsigned int kOtherField = 0x33u;
constexpr unsigned int kOtherWindow = 0x44u;
constexpr unsigned int kModalWindow = 0x55u;
constexpr int kTries = 4;

// A frame with no modal up and nothing holding the caret, the keyboard on `focused`.
unsigned int Idle(CaretReturnTracker& t, unsigned int focused, bool enterHeld = false) {
    return StepCaretReturn(t, false, 0, 0, focused, enterHeld, kTries);
}

// The caret is in kField, then a modal opens and takes the keyboard.
void TypeThenOpenModal(CaretReturnTracker& t) {
    CHECK(StepCaretReturn(t, false, kField, kWindow, kWindow, false, kTries) == 0);
    CHECK(StepCaretReturn(t, true, 0, 0, kModalWindow, false, kTries) == 0);
}

} // namespace

TEST_CASE("StepCaretReturn: the field that had the caret gets it back when the modal closes") {
    CaretReturnTracker t;
    TypeThenOpenModal(t);
    CHECK(StepCaretReturn(t, true, 0, 0, kModalWindow, false, kTries) == 0); // still up: nothing yet
    CHECK(Idle(t, kWindow) == kField);
    // The field took the caret: the request is done and not repeated.
    CHECK(StepCaretReturn(t, false, kField, kWindow, kWindow, false, kTries) == 0);
    CHECK(Idle(t, kWindow) == 0);
}

TEST_CASE("StepCaretReturn: nothing is owed when no field had the caret") {
    CaretReturnTracker t;
    CHECK(Idle(t, kWindow) == 0);
    CHECK(StepCaretReturn(t, true, 0, 0, kModalWindow, false, kTries) == 0);
    CHECK(Idle(t, kWindow) == 0);
}

TEST_CASE("StepCaretReturn: only the field from the frame before the first modal counts") {
    CaretReturnTracker t;
    TypeThenOpenModal(t);
    // A second modal over the first (a confirm over a form) and a field inside a modal change nothing.
    CHECK(StepCaretReturn(t, true, kOtherField, kModalWindow, kModalWindow, false, kTries) == 0);
    CHECK(StepCaretReturn(t, true, 0, 0, kModalWindow, false, kTries) == 0);
    CHECK(Idle(t, kWindow) == kField);
}

TEST_CASE("StepCaretReturn: the caret stays where the dialog's action took the user") {
    SUBCASE("the keyboard is on another window") {
        CaretReturnTracker t;
        TypeThenOpenModal(t);
        CHECK(Idle(t, kOtherWindow) == 0);
        CHECK(Idle(t, kWindow) == 0); // forgotten, not deferred
    }
    SUBCASE("another field has the caret") {
        CaretReturnTracker t;
        TypeThenOpenModal(t);
        CHECK(StepCaretReturn(t, false, kOtherField, kOtherWindow, kOtherWindow, false, kTries) == 0);
        CHECK(Idle(t, kWindow) == 0);
    }
}

TEST_CASE("StepCaretReturn: waits while Enter is held, without spending a try") {
    CaretReturnTracker t;
    TypeThenOpenModal(t);
    for (int i = 0; i < kTries + 2; ++i) {
        CHECK(Idle(t, kWindow, true) == 0);
    }
    CHECK(Idle(t, kWindow) == kField);
}

TEST_CASE("StepCaretReturn: a field that never takes the caret is dropped after the tries") {
    CaretReturnTracker t;
    TypeThenOpenModal(t);
    for (int i = 0; i < kTries; ++i) {
        CHECK(Idle(t, kWindow) == kField);
    }
    CHECK(Idle(t, kWindow) == 0);
    CHECK(Idle(t, kWindow) == 0);
    CHECK(t.ReturnField == 0);
}

TEST_CASE("RequestCaretReturn: an overlay that is not a modal owes the caret back") {
    CaretReturnTracker t;
    RequestCaretReturn(t, kField, kWindow);
    CHECK(Idle(t, kWindow) == kField);
    CHECK(StepCaretReturn(t, false, kField, kWindow, kWindow, false, kTries) == 0);

    SUBCASE("a request without a window owes nothing") {
        CaretReturnTracker u;
        RequestCaretReturn(u, kField, 0);
        CHECK(u.ReturnField == 0);
        CHECK(Idle(u, kWindow) == 0);
    }
    SUBCASE("a new request restarts the tries") {
        CaretReturnTracker u;
        RequestCaretReturn(u, kField, kWindow);
        for (int i = 0; i < kTries; ++i) {
            CHECK(Idle(u, kWindow) == kField);
        }
        RequestCaretReturn(u, kOtherField, kWindow);
        CHECK(Idle(u, kWindow) == kOtherField);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Wave 1 dialog keys: Esc, the caret request and a dismissed popup coming back (moved from
// SmatchetWorklogSubmitPure.test.cpp, where they sat while tests/CMakeLists.txt was plan-locked).
// ---------------------------------------------------------------------------------------------------------

TEST_CASE("ResolveEscape ignores Esc that is not the dialog's") {
    // Not pressed at all.
    CHECK(ResolveEscape(false, true, true, true, true) == EscapeAction::None);
    // A popup above the dialog (suggestions, date picker, templates) has the keyboard: the key is its.
    CHECK(ResolveEscape(true, false, true, false, true) == EscapeAction::None);
    CHECK(ResolveEscape(true, false, false, true, false) == EscapeAction::None);
}

TEST_CASE("ResolveEscape never cancels around typed text in one press") {
    // Caret in a field, something typed: the first Esc only leaves the field.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/true, /*nothingTyped=*/false) == EscapeAction::LeaveField);
    // Out of the field: the next Esc cancels.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/false, /*nothingTyped=*/false) == EscapeAction::Cancel);
}

TEST_CASE("ResolveEscape closes an untouched dialog on the first Esc") {
    // The caret starts in Time spent, so an untouched dialog always has a field active.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/true, /*nothingTyped=*/true) == EscapeAction::Cancel);
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/false, /*nothingTyped=*/true) == EscapeAction::Cancel);
}

TEST_CASE("ResolveEscape does not cancel on the Esc that closed a popup above the dialog") {
    // The popup had the keyboard last frame and closed on this Esc; the dialog only just got it back.
    CHECK(ResolveEscape(true, true, /*dialogOwnedKeyboardLastFrame=*/false, false, true) == EscapeAction::None);
    CHECK(ResolveEscape(true, true, /*dialogOwnedKeyboardLastFrame=*/false, false, false) == EscapeAction::None);
}

TEST_CASE("ResolveCaretRequest waits out a held Enter and a drag, and is dropped under a popup") {
    // Enter still held from the key that opened the dialog, or something being dragged: later.
    CHECK(ResolveCaretRequest(/*enterHeld=*/true, false, true) == CaretRequest::Wait);
    CHECK(ResolveCaretRequest(false, /*dragInProgress=*/true, true) == CaretRequest::Wait);
    // Waiting comes first: a request made under a popup is not dropped while Enter is still down.
    CHECK(ResolveCaretRequest(true, false, /*dialogOwnsKeyboard=*/false) == CaretRequest::Wait);
    // The dialog has the keyboard: the caret goes into the field.
    CHECK(ResolveCaretRequest(false, false, true) == CaretRequest::Take);
    // A popup above the dialog has it: the request is forgotten, not kept for when the popup closes.
    CHECK(ResolveCaretRequest(false, false, false) == CaretRequest::Drop);
}

TEST_CASE("DismissedPopupComesBack brings back typed text once, and does not loop") {
    // Nothing typed: the dismissal stands.
    CHECK_FALSE(DismissedPopupComesBack(/*hasUnsavedText=*/false, false, 100, -1, 2));
    // Typed text, never brought back before: it comes back to ask.
    CHECK(DismissedPopupComesBack(true, false, 100, /*lastReopenFrame=*/-1, 2));
    // Already on its way back this frame: no second request.
    CHECK_FALSE(DismissedPopupComesBack(true, /*comingBackNow=*/true, 100, -1, 2));
    // Dismissed again within the settle window of the last return: something keeps closing it.
    CHECK_FALSE(DismissedPopupComesBack(true, false, 100, 99, 2));
    CHECK_FALSE(DismissedPopupComesBack(true, false, 100, 98, 2));
    // Past the settle window, a new dismissal is the user again.
    CHECK(DismissedPopupComesBack(true, false, 100, 97, 2));
}

// ---------------------------------------------------------------------------------------------------------
// Confirm dialogs and the global-shortcut mute (input-ux-w2-dialog-contract; audit #86, #88). These are the
// rules the shared footer in Ui/SmatchetDialogScope.h reads; kept beside the Wave 1 dialog-key cases.
// ---------------------------------------------------------------------------------------------------------

using smatchet::dialogkeys::ConfirmDefault;
using smatchet::dialogkeys::ConfirmPick;
using smatchet::dialogkeys::GlobalShortcutAllowedUnderModal;
using smatchet::dialogkeys::ResolveConfirmDefault;
using smatchet::dialogkeys::ResolveConfirmEnter;

TEST_CASE("ResolveConfirmDefault: a destructive or consent prompt defaults to the safe button") {
    CHECK(ResolveConfirmDefault(true, false) == ConfirmDefault::Safe);
    CHECK(ResolveConfirmDefault(false, true) == ConfirmDefault::Safe);
    CHECK(ResolveConfirmDefault(true, true) == ConfirmDefault::Safe);
}

TEST_CASE("ResolveConfirmDefault: a non-destructive confirm defaults to its action") {
    CHECK(ResolveConfirmDefault(false, false) == ConfirmDefault::Primary);
}

TEST_CASE("ResolveConfirmEnter: with no keyboard cursor on an item, Enter runs the default button") {
    CHECK(ResolveConfirmEnter(true, true, true, false, ConfirmDefault::Safe) == ConfirmPick::Cancel);
    CHECK(ResolveConfirmEnter(true, true, true, false, ConfirmDefault::Primary) == ConfirmPick::Primary);
}

TEST_CASE("ResolveConfirmEnter: Enter on a destructive confirm never runs the action by default") {
    const ConfirmDefault def = ResolveConfirmDefault(true, false);
    CHECK(ResolveConfirmEnter(true, true, true, false, def) != ConfirmPick::Primary);
}

TEST_CASE("ResolveConfirmEnter: an item under the visible keyboard cursor takes Enter itself") {
    // ImGui activates that item and the footer reads the click, so the decision must not also pick.
    CHECK(ResolveConfirmEnter(true, true, true, true, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, true, true, true, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("ResolveConfirmEnter: nothing without Enter, or while a popup above the dialog has the keyboard") {
    CHECK(ResolveConfirmEnter(false, true, true, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, false, true, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, false, true, false, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("ResolveConfirmEnter: the Enter that opened the dialog does not also answer it") {
    // The dialog opened this frame, so it did not hold the keyboard last frame.
    CHECK(ResolveConfirmEnter(true, true, false, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, true, false, false, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("GlobalShortcutAllowedUnderModal: zoom and fullscreen still run under a modal") {
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.in"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.out"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.reset"));
    CHECK(GlobalShortcutAllowedUnderModal("app.fullscreen.toggle"));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: the focus-scoped gesture commands still run under a modal") {
    CHECK(GlobalShortcutAllowedUnderModal("ui.list.move_up"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.list.remove"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.splitter.grow"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("view.panel.grow")); // names a panel: not what a dialog serves
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.listing.other"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(nullptr));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: everything that changes state is muted, the palette too") {
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.command_palette"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.preferences.toggle"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("view.delete"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("grid.search.focus"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(""));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(nullptr));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: the zoom match needs the whole 'ui.zoom.' segment") {
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.zoom"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.zoomer.in"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("app.fullscreen.toggle.extra"));
}
