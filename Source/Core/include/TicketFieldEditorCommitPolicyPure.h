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

} // namespace TicketFieldEditorCommitPolicyPure
