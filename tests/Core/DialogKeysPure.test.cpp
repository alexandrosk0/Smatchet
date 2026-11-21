// The caret return of the dialog scope (Ui/DialogKeysPure.h: CaretReturnTracker, StepCaretReturn,
// RequestCaretReturn; input-ux-w2-dialog-contract slice 3, audit #24 and #89). The rest of the header's
// rules are covered in SmatchetWorklogSubmitPure.test.cpp, where they landed first.

#include "Ui/DialogKeysPure.h"

#include <doctest/doctest.h>

using smatchet::dialogkeys::CaretReturnTracker;
using smatchet::dialogkeys::RequestCaretReturn;
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
