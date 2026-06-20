#pragma once

// Pure commit-vs-cancel policy for the inline cell text editor (RenderTextInlineEdit in
// TicketFieldEditor.cpp), split out so both platform branches are unit-testable on the desktop
// test build (no ImGui, no platform macro - caller passes the compile-time isMobile value).
// WS4 item 16: desktop commits an inline edit on focus-loss (click-away), but imgui_impl_android
// maps neither Back nor any touch gesture to ImGuiKey_Escape, so the desktop Escape-cancel guard
// is unreachable on-device and every focus-loss (Back, tap-away, IME dismiss) PUT a stray edit to
// the real tracker. Mobile therefore commits ONLY on explicit submit; focus-loss cancels with no
// PUT. Soft-keyboard Enter still arrives as a real ImGuiKey_Enter (SmatchetActivity.java), so
// EnterReturnsTrue fires and on-device editing is preserved.
// estimate-edit-ux follow-up: two more rules, both gated here so they are unit-tested rather than
// re-derived in the ImGui glue. (1) A no-op edit must NEVER PUT — exiting an empty cell that was
// already empty, or clicking an existing value and releasing it unchanged, used to fire a spurious
// (often empty-valued) PUT that the tracker rejected with a toast. (2) Any focus-loss / submit /
// Escape ENDS the edit session whether or not a PUT fires — previously, refocusing an unchanged
// value and then clicking a *different* cell left the editor stuck open because nothing ended it.
namespace TicketFieldEditorCommitPolicyPure {

// True if the active inline edit should COMMIT (QueueEdit / PUT) this frame.
//   explicitSubmit - Enter / IME "Done" (InputText EnterReturnsTrue).
//   deactivated    - the input lost focus this frame (click-away / Back / tap-away), edited or not.
//   isMobile       - touch build, where focus-loss must NOT commit (only explicit submit does).
//   valueChanged   - the edit buffer differs from the field's original value. Gates ALL commits:
//                    a no-op edit never PUTs, regardless of submit / focus-loss / platform.
inline bool ShouldCommitInlineFieldEdit(bool explicitSubmit, bool deactivated, bool isMobile, bool valueChanged) {
    if (!valueChanged) {
        return false; // no-op edit — never PUT (fixes the spurious empty / unchanged-value commit)
    }
    if (explicitSubmit) {
        return true; // Enter / IME Done always commits a real change
    }
    if (isMobile) {
        return false; // mobile: focus-loss must not commit (WS4 item 16)
    }
    return deactivated; // desktop: commit a real change on focus-loss (click-away)
}

// True if the inline edit SESSION should end this frame (the caller commits via
// ShouldCommitInlineFieldEdit when applicable, otherwise just clears the edit state). Escape
// cancels; Enter / IME submit and focus-loss end the session whether or not a PUT fires — so
// clicking another cell after merely refocusing an unchanged value still closes the editor.
inline bool ShouldEndInlineEdit(bool escapePressed, bool explicitSubmit, bool deactivated) {
    return escapePressed || explicitSubmit || deactivated;
}

// P1.3 mobile interaction model (#1018 item 23): the TOUCH-build open-gate for a grid cell editor.
// Touch has no double-click and no hover, so tap-to-select, scroll-fling and edit-open would all
// collapse onto the single tap. Long-press is the standard Android disambiguator: a quick tap
// selects the cell / scrolls; a stationary hold past longPressThresholdSeconds over the cell opens
// the editor; a hold that drifts past the drag threshold is a scroll, not an edit-open. Split out
// here so the gesture rule is unit-tested rather than re-derived in the ImGui glue — the caller
// (TicketFieldEditor.cpp, only when kMobileInlineEditBuild) feeds ImGui state in:
//   cellHovered  - ImGui::IsItemHovered() for the cell Selectable drawn immediately before.
//   primaryDown  - io.MouseDown[0] (the touch contact, mapped to the primary button on Android).
//   heldSeconds  - io.MouseDownDuration[0], ImGui's own per-frame hold timer (no custom clock).
//   dragging     - ImGui::IsMouseDragging(0): held + moved past the drag threshold = scroll.
// Desktop never calls this — it keeps its own click/double-click expression inline so its codegen
// stays byte-identical; this models ONLY the new touch branch.
inline bool ShouldOpenCellEditorByLongPress(bool cellHovered, bool primaryDown, float heldSeconds,
                                            bool dragging, float longPressThresholdSeconds) {
    return cellHovered && primaryDown && heldSeconds >= longPressThresholdSeconds && !dragging;
}

} // namespace TicketFieldEditorCommitPolicyPure
