#ifndef SMATCHET_UI_SMATCHET_INPUT_H
#define SMATCHET_UI_SMATCHET_INPUT_H

// The shared text field (docs/audits/INPUT_UX_AUDIT.md § 4.2, plan docs/plans/input-ux-w2-text-field.md).
// One place owns what a text field does with the keyboard, so Enter, Esc, Ctrl+Enter, the caret on
// open, select-all and trimming mean the same thing in every window (§ 3 keyboard contract):
//
//   Single-line (Text / TextBuf): Enter commits, Ctrl+Enter commits too (and stays the dialog's Save
//   key), Esc reverts to the value the field had when it was activated, Tab and a click elsewhere
//   commit.
//   Multiline (Multiline): Enter types a newline, Ctrl+Enter submits, the first Esc leaves the field
//   and keeps the text. What a second Esc does belongs to the dialog around it.
//
// Text and Multiline keep the value in a std::string that grows as the user types or pastes, so
// nothing is ever clipped. TextBuf is the same field over a caller's fixed char buffer, for storage
// that cannot move to std::string yet; it shows a byte count once the buffer is nearly full.
//
// Labels and hints are source (English) text, translated here the way the SmatchetLocalizedImGui
// wrappers do, so a call site passes the same strings it passed to ImGui::InputText. Plain ::ImGui
// inside: safe to include above or below a TU's `#define ImGui SmatchetLocalizedImGui` line.
//
// Dictation: only TextBuf registers with the dictation router. The router splices into a char
// buffer it keeps a pointer to, and a std::string moves its storage when it grows, so a std::string
// field takes no dictated text until the router can target a string (debt entry
// 2026-11-16-dictation-router-string-target).
//
// UI-thread only. Each call draws exactly one InputText; read the result before drawing anything else.

#include "imgui.h"

#include <cstddef>
#include <string>

namespace SmatchetInput {

/// std::string-backed ImGui::InputText: CallbackResize grows the string as the text grows, so the
/// buffer is never a fixed size. The label is passed to ImGui as given (no translation).
bool InputTextStdString(const char* label, std::string& str, ImGuiInputTextFlags flags = 0);

/// std::string-backed ImGui::InputTextMultiline. The label is passed to ImGui as given.
bool InputTextMultilineStdString(const char* label, std::string& str, const ImVec2& size,
                                 ImGuiInputTextFlags flags = 0);

/// What a single-line field does beyond typing. All off by default.
struct TextOptions {
    /// Placeholder drawn inside the empty field: source text, translated like InputTextWithHint's.
    /// Example values (a host name, a URL) are data and pass through untranslated.
    const char* hint = nullptr;
    /// Put the caret in the field on the first frame it is drawn, with its text selected. Waits while
    /// Enter is held or a drag is under way, and is dropped when another window has the keyboard.
    /// Not for a field inside an ImGuiListClipper: a row that scrolls back in would take the caret.
    bool focusOnAppear = false;
    /// A click into the field selects its text, so typing replaces the value. For values the user
    /// replaces whole (a domain, a URL, a path, a name, a filter), never for prose.
    bool selectAllOnFocus = false;
    /// Strip edge whitespace when the edit ends, except on Esc. For values that cannot carry it.
    bool trimOnCommit = false;
    /// After a bare-Enter commit, keep the caret in the field for the next entry (add-another fields).
    bool refocusAfterEnter = false;
    /// Extra ImGui flags (CharsNoBlank, ...). The field's own flags are added on top.
    ImGuiInputTextFlags flags = 0;
};

/// What happened to a single-line field this frame.
struct TextResult {
    bool edited = false;         ///< the text changed this frame
    bool active = false;         ///< the field has the keyboard (a half-typed value is in it)
    bool committed = false;      ///< the edit ended this frame by anything but Esc
    bool enterCommitted = false; ///< ... on a bare Enter (no modifier): run the field's action
    /// The edit ended on Esc and the field itself took that Esc: the text is back to its value at
    /// activation. When a dialog's DialogReadKeys took the Esc first and left the field, the text is kept
    /// and the end reports as `committed` instead (so trim runs and a caller that saves on `committed` saves).
    bool reverted = false;
};

/// Single-line field over a std::string.
TextResult Text(const char* label, std::string& value, const TextOptions& opts = TextOptions());

/// Single-line field over a caller-owned fixed buffer of `bufSize` bytes (NUL included). Shows a
/// "used / max" byte count beside the field once the buffer is nearly full.
TextResult TextBuf(const char* label, char* buf, std::size_t bufSize, const TextOptions& opts = TextOptions());

/// What a multiline composer does beyond typing.
struct MultilineOptions {
    /// Placeholder drawn inside the empty field (source text, translated). Say the submit key in it.
    const char* hint = nullptr;
    /// As TextOptions::focusOnAppear, except nothing is selected (ImGui never selects a multiline
    /// field's text on focus), so a draft is added to, not replaced.
    bool focusOnAppear = false;
    /// Ctrl+Enter reports `submitted`. When off, Ctrl+Enter only leaves the field (ImGui ends the edit).
    bool submitOnCtrlEnter = true;
    /// Extra ImGui flags.
    ImGuiInputTextFlags flags = 0;
};

/// What happened to a multiline composer this frame.
struct MultilineResult {
    bool edited = false;    ///< the text changed this frame
    bool active = false;    ///< the field has the keyboard
    bool submitted = false; ///< Ctrl+Enter ended the edit: send / save the text
    /// Esc took the field off the keyboard; the text is kept as typed. A dialog that also closes on Esc
    /// treats this frame's Esc as used up: the field has already left the keyboard, so the dialog's own
    /// "is a field active" check reads false on the very frame the key was pressed.
    bool leftField = false;
};

/// Multiline composer over a std::string.
MultilineResult Multiline(const char* label, std::string& value, const ImVec2& size,
                          const MultilineOptions& opts = MultilineOptions());

} // namespace SmatchetInput

#endif // SMATCHET_UI_SMATCHET_INPUT_H
