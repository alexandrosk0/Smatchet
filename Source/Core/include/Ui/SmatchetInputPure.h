#pragma once

/// Pure decisions behind the shared text field (Ui/SmatchetInput.h). No ImGui here: the widget reads
/// the keys, the item state and the frame counter, and these say what follows. Kept apart so the
/// doctest rig can pin every rule without an ImGui context (docs/audits/INPUT_UX_AUDIT.md § 3, § 4.2).

#include <cstddef>
#include <cstring>
#include <string>

namespace smatchet {
namespace textinput {

/// The body of the std::string resize callback: grow (or shrink) `s` to `newLength` bytes and hand
/// back the storage ImGui writes into. A negative length (never sent by ImGui, but the field is an
/// int) is read as 0. The pointer is taken through c_str() as imgui_stdlib does, so it stays valid
/// under C++14, where std::string has no non-const data().
inline char* GrowStringForEdit(std::string& s, int newLength) {
    s.resize(newLength > 0 ? static_cast<std::size_t>(newLength) : 0u);
    return const_cast<char*>(s.c_str());
}

/// The edge whitespace a committed value sheds: space, tab, CR, LF (the TrimCopyAsciiWhitespace set).
inline bool IsTrimmedEdgeChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Strips edge whitespace in place. True when it changed `s`.
inline bool TrimEdgeWhitespace(std::string& s) {
    std::size_t start = 0;
    std::size_t end = s.size();
    while (start < end && IsTrimmedEdgeChar(s[start])) {
        ++start;
    }
    while (end > start && IsTrimmedEdgeChar(s[end - 1])) {
        --end;
    }
    if (start == 0 && end == s.size()) {
        return false;
    }
    s = s.substr(start, end - start);
    return true;
}

/// The same over a caller-owned fixed buffer. Reads at most `bufSize` bytes and always leaves the
/// buffer NUL-terminated within them, so an unterminated buffer is cut at its last byte instead of
/// read past. True when it changed the buffer. A null or zero-size buffer is left alone.
inline bool TrimEdgeWhitespace(char* buf, std::size_t bufSize) {
    if (buf == nullptr || bufSize == 0u) {
        return false;
    }
    std::size_t len = 0;
    while (len < bufSize && buf[len] != '\0') {
        ++len;
    }
    bool changed = false;
    if (len == bufSize) {
        buf[bufSize - 1u] = '\0';
        len = bufSize - 1u;
        changed = true;
    }
    std::size_t start = 0;
    std::size_t end = len;
    while (start < end && IsTrimmedEdgeChar(buf[start])) {
        ++start;
    }
    while (end > start && IsTrimmedEdgeChar(buf[end - 1])) {
        --end;
    }
    if (start == 0 && end == len) {
        return changed;
    }
    const std::size_t kept = end - start;
    if (start != 0 && kept != 0) {
        std::memmove(buf, buf + start, kept);
    }
    buf[kept] = '\0';
    return true;
}

/// True when a field drawn on `frame` was not drawn on the frame before: it has just appeared (a
/// Rename swapped it in, its popup opened, its window came back). `lastDrawnFrame` is negative for a
/// field never drawn. Behind `focusOnAppear`, so a field inside an ImGuiListClipper (which stops
/// drawing rows that scroll out) must not use it.
inline bool FieldAppearedThisFrame(int lastDrawnFrame, int frame) {
    return lastDrawnFrame < 0 || lastDrawnFrame != frame - 1;
}

/// How a single-line edit ended this frame (all false while it goes on or never started).
struct SingleLineEnd {
    bool committed;      ///< ended by anything but an Esc the field took: Enter, Tab, a click elsewhere
    bool enterCommitted; ///< ... and on a bare Enter, the key a form runs its action on
    bool reverted;       ///< ended on Esc; ImGui put back the text the field had when activated
};

/// An Esc the field itself took reverts; everything else that ends the edit commits (§ 3 single-line row).
/// `escPressed` is false when a surrounding scope took the Esc first and left the field with its text kept,
/// since nothing was reverted then. Only a bare Enter
/// (`bareEnterPressed`: no Ctrl / Shift / Alt / Super) counts as the field's Enter: Ctrl+Enter also
/// ends a single-line edit, and commits it, but stays the surrounding dialog's Save key.
inline SingleLineEnd ResolveSingleLineEnd(bool deactivated, bool escPressed, bool bareEnterPressed) {
    SingleLineEnd end = {false, false, false};
    if (!deactivated) {
        return end;
    }
    if (escPressed) {
        end.reverted = true;
        return end;
    }
    end.committed = true;
    end.enterCommitted = bareEnterPressed;
    return end;
}

/// § 3 multiline row: the first Esc leaves the field and keeps the text. ImGui's own Esc reverts the
/// whole edit, so the widget takes the field off the keyboard before ImGui sees the key.
inline bool MultilineEscLeavesField(bool fieldActive, bool escPressed) { return fieldActive && escPressed; }

/// § 3 multiline row: Ctrl+Enter submits; Enter and Shift+Enter type a newline. ImGui ends a
/// multiline edit on Ctrl+Enter, so it is a submit when the edit ended this frame with Enter (or
/// keypad Enter) pressed, Ctrl held and no other modifier, and the caller wants Ctrl+Enter to submit.
inline bool MultilineSubmitted(bool deactivated, bool enterPressed, bool ctrlHeld, bool shiftHeld, bool altHeld,
                               bool superHeld, bool submitEnabled) {
    return submitEnabled && deactivated && enterPressed && ctrlHeld && !shiftHeld && !altHeld && !superHeld;
}

/// True when a fixed buffer is full enough that its byte count should show beside the field: within
/// a tenth of its usable bytes (one is the NUL) of the end. Without it ImGui drops typed or pasted
/// text past the end with no cue (audit #2, #132). Written without multiplying, so no size overflows.
inline bool ShowCapacityCounter(std::size_t used, std::size_t bufSize) {
    if (bufSize < 2u) {
        return bufSize == 1u; // a 1-byte buffer holds nothing: it is always full
    }
    const std::size_t usable = bufSize - 1u;
    return used >= usable - usable / 10u;
}

} // namespace textinput
} // namespace smatchet
