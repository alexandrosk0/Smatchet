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
namespace TicketFieldEditorCommitPolicyPure {

// True if the active inline edit should commit (QueueEdit) this frame. explicitSubmit = Enter /
// IME "Done" (InputText EnterReturnsTrue); deactivatedAfterEdit = widget lost focus after an edit
// (click-away / Back / tap-away); isMobile = touch build, where focus-loss must NOT commit.
inline bool ShouldCommitInlineFieldEdit(bool explicitSubmit, bool deactivatedAfterEdit, bool isMobile) {
    if (explicitSubmit) {
        return true;
    }
    if (isMobile) {
        return false;
    }
    return deactivatedAfterEdit;
}

} // namespace TicketFieldEditorCommitPolicyPure
