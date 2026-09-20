#pragma once

// Pure, ImGui-free decision core for the drag-to-paint checkbox gesture: press a checkbox
// and drag across its neighbours to set every row the pointer crosses to the value the
// press produced (uncheck-drag when the first row was checked, check-drag when it was not).
// The impure wrapper (SmatchetDragCheckbox.h) feeds live hit-testing in and applies the
// decisions to the caller's bool; keeping the state machine here lets tests/Core pin the
// gesture contract without an ImGui context.
//
// Frame ordering the wrapper guarantees, and that these decisions assume:
//   press frame   — the host widget has NOT toggled yet (Dear ImGui's Checkbox toggles on
//                   release), so the press is what opens the gesture and writes the value.
//   drag frames   — only this state machine writes; the host widget never sees the other
//                   rows as hovered while the pressed row owns the active id.
//   release frame — the host widget fires its own toggle if the release lands back on the
//                   origin row. That toggle would undo the press, so it is reverted here.

namespace SmatchetDragCheckboxPure {

// The in-flight gesture. One instance per UI thread, owned by the wrapper.
struct PaintGesture {
    unsigned int Scope = 0;  // list that owns the gesture (the drawing window's id)
    unsigned int Origin = 0; // row the press landed on
    bool Active = false;
    bool Value = false; // the value being painted across the list
    int Frame = -1;     // last frame a row of this list serviced the gesture
};

// One row's live input for this frame.
struct ItemFrame {
    unsigned int Scope = 0;
    unsigned int Id = 0;
    bool Value = false;       // the row's value BEFORE the host widget applied its own click
    bool Pressed = false;     // a left press landed on this row this frame
    bool DraggedOver = false; // the button is held and the pointer crossed this row's hit band
    bool Clicked = false;     // the host widget reported its own click-release toggle
    bool Disabled = false;    // the row is not interactive this frame (BeginDisabled / locked)
};

// What the wrapper does with the row after DecideItem.
struct ItemDecision {
    bool Begin = false;   // open a gesture owned by this row (Value is what it paints)
    bool Write = false;   // store Value back into the caller's bool
    bool Value = false;   // the value to store
    bool Changed = false; // report a user-visible toggle to the caller
};

// A gesture lives while the button is down, PLUS the one frame the button comes up on: Dear
// ImGui reports its own click on the release frame (by which time the button already reads
// up), and the origin row needs the gesture still in hand to swallow that second toggle —
// drop it a frame early and every plain click becomes a press-toggle the release undoes.
// A frame gap ends it regardless: the list stopped rendering under the pointer (tab switched,
// popup closed, window hidden) and the gesture is stranded, so resuming a paint into a list
// the user has since left is never right. `frame` is the current frame counter.
inline bool GestureLapsed(const PaintGesture& gesture, bool mouseDown, bool mouseReleased, bool mousePressed,
                          int frame) {
    if (!gesture.Active) {
        return false;
    }
    if (frame > gesture.Frame + 1) {
        return true;
    }
    if (mousePressed && gesture.Frame != frame) {
        // A fresh press while the previous run is still in its release frame (a fast
        // double-click, or queued input delivered a frame apart) starts a NEW run — without
        // this the stale run owns the pointer and swallows that press entirely. The
        // frame guard keeps the press that OPENED this run from ending it: the wrapper
        // stamps Frame on the opening frame, so `Frame == frame` means "started right here".
        return true;
    }
    return !mouseDown && !mouseReleased;
}

// True when the pointer's travel between the previous and current frame crossed the row's
// vertical band. Sampling the instantaneous position alone drops rows on a fast flick (the
// pointer jumps several row heights per frame at 144 Hz), which leaves holes in the painted
// run; the swept segment closes them.
inline bool SegmentCrossesBand(float prevY, float curY, float bandMinY, float bandMaxY) {
    const float lo = prevY < curY ? prevY : curY;
    const float hi = prevY < curY ? curY : prevY;
    return hi >= bandMinY && lo <= bandMaxY;
}

// The whole per-row decision. `gesture` is the state BEFORE this row is serviced.
inline ItemDecision DecideItem(const PaintGesture& gesture, const ItemFrame& item) {
    ItemDecision decision;
    if (item.Disabled) {
        // A disabled row neither opens a gesture nor takes paint — dragging across a locked
        // row (the Views "ID (locked)" checkbox) must leave it untouched.
        return decision;
    }
    if (item.Pressed && !gesture.Active) {
        decision.Begin = true;
        decision.Write = true;
        decision.Value = !item.Value;
        decision.Changed = true;
        return decision;
    }
    const bool inScope = gesture.Active && gesture.Scope == item.Scope;
    // Paint only rows that actually differ, so a drag back over an already-painted row never
    // re-reports a change (callers set dirty flags / queue edits off Changed).
    if (inScope && item.Id != gesture.Origin && item.DraggedOver && item.Value != gesture.Value) {
        decision.Write = true;
        decision.Value = gesture.Value;
        decision.Changed = true;
        return decision;
    }
    if (inScope && item.Id == gesture.Origin && item.Clicked) {
        // Release landed back on the origin: the press already applied the toggle, so the
        // host widget's release-toggle is a double-toggle. Restore the painted value and
        // report nothing — the change was reported on the press frame.
        decision.Write = true;
        decision.Value = gesture.Value;
        return decision;
    }
    if (item.Clicked) {
        // A toggle this gesture never owned (keyboard Space / gamepad): the host widget has
        // already written it, so only the change report is ours.
        decision.Changed = true;
    }
    return decision;
}

} // namespace SmatchetDragCheckboxPure
