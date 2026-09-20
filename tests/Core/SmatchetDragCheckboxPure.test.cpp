// Pure-logic coverage for the drag-to-paint checkbox gesture (SmatchetDragCheckboxPure.h) —
// click a checkbox and drag across its neighbours to set the whole run to one value. These
// pin the contract the ImGui wrapper (SmatchetDragCheckbox.h) feeds live hit-testing into:
// the press owns the value, Dear ImGui's own release-toggle on the origin row must not
// double-toggle it, disabled rows are inert, a run never leaves its list, and a stranded
// gesture (button up, or the list stopped drawing) is dropped instead of resumed.

#include "SmatchetDragCheckboxPure.h"

#include <doctest/doctest.h>

namespace pure = SmatchetDragCheckboxPure;

namespace {

const unsigned int kListA = 1001;
const unsigned int kListB = 2002;
const unsigned int kRowOne = 11;
const unsigned int kRowTwo = 22;

// A gesture as the wrapper leaves it after a press on `origin` painted `value`.
pure::PaintGesture ActiveGesture(unsigned int scope, unsigned int origin, bool value, int frame = 7) {
    pure::PaintGesture gesture;
    gesture.Active = true;
    gesture.Scope = scope;
    gesture.Origin = origin;
    gesture.Value = value;
    gesture.Frame = frame;
    return gesture;
}

pure::ItemFrame Row(unsigned int id, bool value, unsigned int scope = kListA) {
    pure::ItemFrame item;
    item.Scope = scope;
    item.Id = id;
    item.Value = value;
    return item;
}

} // namespace

TEST_CASE("A press opens the gesture and writes the toggled value") {
    const pure::PaintGesture idle;
    pure::ItemFrame item = Row(kRowOne, false);
    item.Pressed = true;

    const pure::ItemDecision decision = pure::DecideItem(idle, item);
    CHECK(decision.Begin);
    CHECK(decision.Write);
    CHECK(decision.Value); // unchecked row -> the run paints "checked"
    CHECK(decision.Changed);

    // Pressing a CHECKED row paints the run "unchecked" — drag-to-clear.
    pure::ItemFrame checkedRow = Row(kRowOne, true);
    checkedRow.Pressed = true;
    const pure::ItemDecision clearing = pure::DecideItem(idle, checkedRow);
    CHECK(clearing.Begin);
    CHECK(clearing.Write);
    CHECK_FALSE(clearing.Value);
    CHECK(clearing.Changed);
}

TEST_CASE("Dragging over a neighbour paints it once, and only when it differs") {
    const pure::PaintGesture gesture = ActiveGesture(kListA, kRowOne, true);

    pure::ItemFrame unchecked = Row(kRowTwo, false);
    unchecked.DraggedOver = true;
    const pure::ItemDecision painted = pure::DecideItem(gesture, unchecked);
    CHECK_FALSE(painted.Begin); // the run keeps its original owner
    CHECK(painted.Write);
    CHECK(painted.Value);
    CHECK(painted.Changed);

    // Dragging back over a row the run already painted must not re-report a change — call
    // sites set dirty flags / queue tracker edits off Changed.
    pure::ItemFrame already = Row(kRowTwo, true);
    already.DraggedOver = true;
    const pure::ItemDecision repeat = pure::DecideItem(gesture, already);
    CHECK_FALSE(repeat.Write);
    CHECK_FALSE(repeat.Changed);

    // Not under the pointer: untouched.
    const pure::ItemDecision missed = pure::DecideItem(gesture, Row(kRowTwo, false));
    CHECK_FALSE(missed.Write);
    CHECK_FALSE(missed.Changed);
}

TEST_CASE("A release back on the origin row is not a second toggle") {
    // Dear ImGui's Checkbox fires on release; the press already applied the value, so the
    // wrapper has to restore it and stay silent (the change was reported on the press frame).
    const pure::PaintGesture gesture = ActiveGesture(kListA, kRowOne, true);
    pure::ItemFrame origin = Row(kRowOne, true);
    origin.Clicked = true;

    const pure::ItemDecision decision = pure::DecideItem(gesture, origin);
    CHECK(decision.Write);
    CHECK(decision.Value); // restored to the painted value, undoing ImGui's release-toggle
    CHECK_FALSE(decision.Changed);
}

TEST_CASE("A toggle the gesture never owned is reported, not rewritten") {
    // Keyboard Space with no gesture in flight: ImGui already wrote the value.
    const pure::PaintGesture idle;
    pure::ItemFrame item = Row(kRowOne, false);
    item.Clicked = true;
    const pure::ItemDecision decision = pure::DecideItem(idle, item);
    CHECK_FALSE(decision.Write);
    CHECK(decision.Changed);
}

TEST_CASE("Disabled rows neither open a run nor take paint") {
    pure::ItemFrame locked = Row(kRowTwo, false);
    locked.Disabled = true;
    locked.DraggedOver = true;
    const pure::ItemDecision painted = pure::DecideItem(ActiveGesture(kListA, kRowOne, true), locked);
    CHECK_FALSE(painted.Write);
    CHECK_FALSE(painted.Changed);

    pure::ItemFrame lockedPress = Row(kRowTwo, false);
    lockedPress.Disabled = true;
    lockedPress.Pressed = true;
    const pure::ItemDecision pressed = pure::DecideItem(pure::PaintGesture(), lockedPress);
    CHECK_FALSE(pressed.Begin);
    CHECK_FALSE(pressed.Write);
    CHECK_FALSE(pressed.Changed);
}

TEST_CASE("A run never paints a list the pointer merely passes over") {
    const pure::PaintGesture gesture = ActiveGesture(kListA, kRowOne, true);
    pure::ItemFrame otherList = Row(kRowTwo, false, kListB);
    otherList.DraggedOver = true;
    const pure::ItemDecision decision = pure::DecideItem(gesture, otherList);
    CHECK_FALSE(decision.Write);
    CHECK_FALSE(decision.Changed);
}

TEST_CASE("A press elsewhere never hijacks a run already in flight") {
    // Defensive: the wrapper drops a lapsed gesture first, so this is the stale-state path.
    const pure::PaintGesture gesture = ActiveGesture(kListA, kRowOne, true);
    pure::ItemFrame press = Row(kRowTwo, true, kListB);
    press.Pressed = true;
    const pure::ItemDecision decision = pure::DecideItem(gesture, press);
    CHECK_FALSE(decision.Begin); // the in-flight run still owns the pointer
    CHECK_FALSE(decision.Write);
    CHECK_FALSE(decision.Changed);
}

TEST_CASE("GestureLapsed keeps the run through the release frame, then drops it") {
    const pure::PaintGesture gesture = ActiveGesture(kListA, kRowOne, true, 7);
    CHECK_FALSE(pure::GestureLapsed(gesture, true, false, 7)); // serviced this frame
    CHECK_FALSE(pure::GestureLapsed(gesture, true, false, 8)); // next frame, still drawing
    // The release frame reads button-up, and it is the frame ImGui reports its own toggle on.
    // Lapsing here would leave that toggle to undo what the press applied — every plain click
    // would be a no-op reported twice.
    CHECK_FALSE(pure::GestureLapsed(gesture, false, true, 8));
    CHECK(pure::GestureLapsed(gesture, false, false, 8));                   // button up, release already consumed
    CHECK(pure::GestureLapsed(gesture, true, false, 9));                    // the list stopped drawing — stranded
    CHECK(pure::GestureLapsed(gesture, false, true, 9));                    // a frame gap ends it either way
    CHECK_FALSE(pure::GestureLapsed(pure::PaintGesture(), true, false, 9)); // nothing in flight
}

TEST_CASE("A whole plain click nets exactly one toggle, reported once") {
    // The press/release pair as the wrapper presents it: the press frame opens the run and
    // writes, and the release frame (gesture still alive) restores that value in place of
    // ImGui's own toggle. This is the sequence the release-frame lifecycle exists for.
    bool row = false;
    pure::PaintGesture gesture;
    int reported = 0;

    pure::ItemFrame press = Row(kRowOne, row);
    press.Pressed = true;
    pure::ItemDecision decision = pure::DecideItem(gesture, press);
    if (decision.Write) {
        row = decision.Value;
    }
    if (decision.Begin) {
        gesture = ActiveGesture(kListA, kRowOne, decision.Value, 7);
    }
    reported += decision.Changed ? 1 : 0;
    CHECK(row);

    // Release frame: ImGui's Checkbox has already flipped the caller's bool back.
    row = !row;
    CHECK_FALSE(pure::GestureLapsed(gesture, false, true, 8));
    pure::ItemFrame release = Row(kRowOne, row);
    release.Clicked = true;
    decision = pure::DecideItem(gesture, release);
    if (decision.Write) {
        row = decision.Value;
    }
    reported += decision.Changed ? 1 : 0;

    CHECK(row);            // the click stuck
    CHECK_EQ(reported, 1); // and the caller was told about it exactly once
}

TEST_CASE("SegmentCrossesBand sweeps the pointer path so a fast flick leaves no holes") {
    // A row spanning [100, 120] in window space.
    CHECK(pure::SegmentCrossesBand(110.0f, 112.0f, 100.0f, 120.0f)); // pointer inside
    CHECK(pure::SegmentCrossesBand(40.0f, 300.0f, 100.0f, 120.0f));  // flicked straight past it
    CHECK(pure::SegmentCrossesBand(300.0f, 40.0f, 100.0f, 120.0f));  // and upward
    CHECK(pure::SegmentCrossesBand(90.0f, 100.0f, 100.0f, 120.0f));  // touching the top edge
    CHECK(pure::SegmentCrossesBand(120.0f, 130.0f, 100.0f, 120.0f)); // touching the bottom edge
    CHECK_FALSE(pure::SegmentCrossesBand(40.0f, 99.0f, 100.0f, 120.0f));
    CHECK_FALSE(pure::SegmentCrossesBand(121.0f, 300.0f, 100.0f, 120.0f));
}
