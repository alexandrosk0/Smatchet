#pragma once

#include <cstddef>

// Pure focus/commit decisions for the duration-suggestions editor (DrawDurationFieldWithSuggestions
// in TicketFieldEditor.cpp), split out so the popup-close and type-to-edit policies are
// unit-testable on the desktop test build (no ImGui — the caller passes the queried widget state).
// estimate-edit-ux: opening the suggestions popup focuses the popup window, which clears the
// parent ActiveId, so the InputText silently loses keyboard focus. Two consequences fixed here.
// First, a printable keystroke with no active item was dropped — and SetKeyboardFocusHere() alone
// cannot recover it: it is a nav-move applied NEXT frame, io.InputQueueCharacters is cleared at
// EndFrame, and on the activation frame a nav-requested activation makes InputText skip insertion
// and clear the queue anyway. So the caller splices the queued printable chars directly into the
// buffer (SpliceTypedCharsIntoBuf below) and uses SetKeyboardFocusHere() for focus only.
// Second, a click that closes the popup but lands back on the input itself is a refocus-for-editing,
// NOT a commit-deactivation — treating it as one queued a spurious (often empty-value) PUT.
namespace TicketFieldEditorDurationPopupPure {

// Printable typed character (one that should land in the input). Filters ALL control codes below
// 0x20 plus DEL (0x7F) — this covers the navigation/dismiss keys the Win32 backend queues via
// WM_CHAR (\r Enter, \x1b Escape, \b Backspace, \t Tab) and also the control codes Ctrl+letter
// combos produce as WM_CHAR — those keep their existing key-handling behaviour.
inline bool IsPrintableTypedChar(unsigned int c) { return c >= 0x20 && c != 0x7F; }

// True if a printable keystroke should pull keyboard focus into the duration InputText this frame
// (type-to-edit). The caller gates on its typeToEditFocus opt-in before asking (only the grid
// inline-edit path opts in; the worklog dialog has sibling text inputs and keeps stock focus
// behaviour). anyItemActive: some widget already owns the keystroke (e.g. another InputText is
// consuming the queue) — never steal it. focusAlreadyPending: a reposition/focus request is
// already armed for this frame.
inline bool ShouldPullFocusForTypedChar(bool anyItemActive, bool focusAlreadyPending, bool hasPrintableQueuedChar) {
    return !anyItemActive && !focusAlreadyPending && hasPrintableQueuedChar;
}

// Appends the printable chars of a typed-character queue snapshot onto the end of buf as UTF-8.
// A minimal encoder lives here so the header stays ImGui-free for the desktop test build; it is
// equivalent to ImTextCharToUtf8 for valid scalar values. Non-printables and codepoints above
// U+10FFFF are skipped; encoding stops at the first char that would not fit alongside the
// terminator, keeping whatever already landed. buf stays NUL-terminated. Returns true if at least
// one char landed — the caller then marks the buffer manually-edited and arms the caret-to-end
// focus return. See the header comment above for why a direct splice (not next-frame queue
// replay) is required.
inline bool SpliceTypedCharsIntoBuf(const unsigned int* queueChars, int queueCount, char* buf, size_t bufSize) {
    if (buf == nullptr || bufSize == 0 || queueChars == nullptr) {
        return false;
    }
    size_t len = 0;
    while (len < bufSize && buf[len] != '\0') {
        ++len;
    }
    if (len >= bufSize) {
        return false; // unterminated input buffer — refuse rather than write past the end
    }
    bool splicedAny = false;
    for (int i = 0; i < queueCount; ++i) {
        const unsigned int c = queueChars[i];
        if (!IsPrintableTypedChar(c) || c > 0x10FFFF) {
            continue;
        }
        char enc[4];
        size_t n = 0;
        if (c < 0x80) {
            enc[n++] = static_cast<char>(c);
        } else if (c < 0x800) {
            enc[n++] = static_cast<char>(0xC0 | (c >> 6));
            enc[n++] = static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            enc[n++] = static_cast<char>(0xE0 | (c >> 12));
            enc[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            enc[n++] = static_cast<char>(0x80 | (c & 0x3F));
        } else {
            enc[n++] = static_cast<char>(0xF0 | (c >> 18));
            enc[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            enc[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            enc[n++] = static_cast<char>(0x80 | (c & 0x3F));
        }
        if (len + n + 1 > bufSize) {
            break; // would not fit with the terminator — keep what already landed
        }
        for (size_t k = 0; k < n; ++k) {
            buf[len++] = enc[k];
        }
        splicedAny = true;
    }
    buf[len] = '\0';
    return splicedAny;
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

// True if the popup-close should instead re-arm the input's focus-return: the close ate a click
// that landed on the input (hovered) without re-activating it, so the input must regain keyboard
// focus next frame. NOT simply the negation of ShouldFinalizeOnPopupClose — it additionally
// requires hovered-and-not-active (a click that re-activated the input naturally needs no re-arm),
// and stays false while a focus-return is already pending.
inline bool ShouldRearmFocusOnPopupClose(bool popupJustClosed, bool focusReturnPending, bool inputHoveredNow,
                                         bool inputActiveNow) {
    return popupJustClosed && !focusReturnPending && inputHoveredNow && !inputActiveNow;
}

} // namespace TicketFieldEditorDurationPopupPure
