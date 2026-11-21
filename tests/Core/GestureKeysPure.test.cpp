// Pure-logic tests for the gesture-key latch (Ui/GestureKeysPure.h): the one-shot request the ui.list.* and
// ui.splitter.* commands post and the focused widget takes.

#include <doctest/doctest.h>

#include "Ui/GestureKeysPure.h"

using smatchet::ui::GestureKey;
using smatchet::ui::GestureKeyLatch;
using smatchet::ui::IsFocusScopedGestureCommand;

TEST_CASE("GestureKeyLatch: a request is taken once, in the frame it was posted") {
    GestureKeyLatch latch;
    CHECK_FALSE(latch.Take(GestureKey::ListMoveUp, 10)); // nothing posted yet
    latch.Post(GestureKey::ListMoveUp, 10);
    CHECK(latch.Take(GestureKey::ListMoveUp, 10));
    CHECK_FALSE(latch.Take(GestureKey::ListMoveUp, 10)); // consumed: one press, one move
}

TEST_CASE("GestureKeyLatch: the request lives one more frame for a widget drawn before the dispatch") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::ListRemove, 10);
    CHECK(latch.Pending(GestureKey::ListRemove, 11));
    CHECK(latch.Take(GestureKey::ListRemove, 11));
}

TEST_CASE("GestureKeyLatch: a request nobody took expires") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::SplitterGrow, 10);
    CHECK_FALSE(latch.Pending(GestureKey::SplitterGrow, 12));
    CHECK_FALSE(latch.Take(GestureKey::SplitterGrow, 12));
    // An expired request is gone, not queued for a later widget.
    CHECK_FALSE(latch.Take(GestureKey::SplitterGrow, 10));
}

TEST_CASE("GestureKeyLatch: a request stamped after the asking frame is dropped (frame counter reset)") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::SplitterShrink, 500);
    CHECK_FALSE(latch.Take(GestureKey::SplitterShrink, 3));
}

TEST_CASE("GestureKeyLatch: keys are independent, and a second post replaces the first") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::ListMoveUp, 5);
    latch.Post(GestureKey::ListMoveDown, 5);
    CHECK(latch.Take(GestureKey::ListMoveDown, 5));
    CHECK(latch.Take(GestureKey::ListMoveUp, 5));

    latch.Post(GestureKey::ListMoveUp, 5);
    latch.Post(GestureKey::ListMoveUp, 5); // two presses in a frame still move one place
    CHECK(latch.Take(GestureKey::ListMoveUp, 5));
    CHECK_FALSE(latch.Take(GestureKey::ListMoveUp, 5));
}

TEST_CASE("GestureKeyLatch: Pending does not consume, Clear drops everything") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::ListRemove, 7);
    CHECK(latch.Pending(GestureKey::ListRemove, 7));
    CHECK(latch.Pending(GestureKey::ListRemove, 7));
    latch.Clear();
    CHECK_FALSE(latch.Pending(GestureKey::ListRemove, 7));
    CHECK_FALSE(latch.Take(GestureKey::ListRemove, 7));
}

TEST_CASE("GestureKeyLatch: an out-of-range key is ignored, not a write past the array") {
    GestureKeyLatch latch;
    latch.Post(GestureKey::Count, 1);
    latch.Post(static_cast<GestureKey>(-1), 1);
    CHECK_FALSE(latch.Take(GestureKey::Count, 1));
    CHECK_FALSE(latch.Take(static_cast<GestureKey>(-1), 1));
    CHECK_FALSE(latch.Pending(GestureKey::Count, 1));
}

TEST_CASE("IsFocusScopedGestureCommand: the list and splitter commands, and nothing near them") {
    CHECK(IsFocusScopedGestureCommand("ui.list.move_up"));
    CHECK(IsFocusScopedGestureCommand("ui.list.remove"));
    CHECK(IsFocusScopedGestureCommand("ui.splitter.grow"));
    CHECK_FALSE(IsFocusScopedGestureCommand("view.panel.grow")); // names its panel, not the focused widget
    CHECK_FALSE(IsFocusScopedGestureCommand("ui.zoom.in"));
    CHECK_FALSE(IsFocusScopedGestureCommand("ui.list"));
    CHECK_FALSE(IsFocusScopedGestureCommand(""));
    CHECK(IsFocusScopedGestureCommand("ui.list.move_down")); // the C-string overload the dialog contract calls
    CHECK_FALSE(IsFocusScopedGestureCommand(static_cast<const char*>(nullptr)));
}
