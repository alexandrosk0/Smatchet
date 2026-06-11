#pragma once

// Pure focus/commit decisions for the duration-suggestions editor (DrawDurationFieldWithSuggestions
// in TicketFieldEditor.cpp), split out so the popup-close and type-to-edit policies are
// unit-testable on the desktop test build (no ImGui — the caller passes the queried widget state).
// estimate-edit-ux: opening the suggestions popup focuses the popup window, which clears the
// parent ActiveId, so the InputText silently loses keyboard focus. Two consequences fixed here.
// First, a printable keystroke with no active item was dropped — it must pull focus back into the
// input; ImGui replays the still-queued character into the input activated that same frame.
// Second, a click that closes the popup but lands back on the input itself is a refocus-for-editing,
// NOT a commit-deactivation — treating it as one queued a spurious (often empty-value) PUT.
namespace TicketFieldEditorDurationPopupPure {

// Printable typed character (one that should land in the input). Filters the control chars the
// Win32 backend queues via WM_CHAR for non-text keys: \r (Enter), \x1b (Escape), \b (Backspace),
// \t (Tab), and DEL — navigation/dismiss keys keep their existing behaviour.
inline bool IsPrintableTypedChar(unsigned int c) { return c >= 0x20 && c != 0x7F; }

// True if a printable keystroke should pull keyboard focus into the duration InputText this frame
// (type-to-edit). typeToEditEnabled: only the grid inline-edit path opts in (the worklog dialog
// has sibling text inputs and keeps stock focus behaviour). anyItemActive: some widget already
// owns the keystroke (e.g. another InputText is consuming the queue) — never steal it.
// focusAlreadyPending: a reposition/focus request is already armed for this frame.
inline bool ShouldPullFocusForTypedChar(bool typeToEditEnabled, bool anyItemActive, bool focusAlreadyPending,
                                        bool hasPrintableQueuedChar) {
    return typeToEditEnabled && !anyItemActive && !focusAlreadyPending && hasPrintableQueuedChar;
}

// True if the suggestions popup closing this frame should finalize (commit-deactivate) the edit.
// A close whose click landed back on the input itself — inputActive when the click re-activated
// it naturally (caret placed by the click), or inputHovered when the popup-close ate the click —
// is a refocus-for-editing and must NOT commit. Intentional clear-the-field commits (Enter or a
// genuine click-away with an empty buffer) are unaffected: they do not arrive via this path.
inline bool ShouldFinalizeOnPopupClose(bool popupJustClosed, bool focusReturnPending, bool inputActive,
                                       bool inputHovered) {
    return popupJustClosed && !focusReturnPending && !inputActive && !inputHovered;
}

} // namespace TicketFieldEditorDurationPopupPure
