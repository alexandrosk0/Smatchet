#ifndef SMATCHET_UI_IMGUI_HOTKEY_H
#define SMATCHET_UI_IMGUI_HOTKEY_H

#include <string>
#include <vector>

#include "imgui.h"

// ImGuiHotkey — parse a human hotkey string ("Ctrl+Shift+B") into modifier flags
// + an ImGuiKey, and match it against the current ImGui input frame. Used by the
// in-app "Log a Bug" hotkey (config: BugReportHotkey). NOT the Whisper Win32-VK
// HotkeyParse (that's a SMATCHET_WITH_WHISPER plugin using OS-global registration).
// docs/plans/shipped/log-a-bug-github.md Slice 4.

namespace smatchet {
namespace ui {

struct ImGuiBugHotkey {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;
    ImGuiKey key = ImGuiKey_None;
};

/// Parse "Ctrl+Shift+B" (case-insensitive, '+'-separated). Recognises modifiers
/// ctrl/control, shift, alt, super/win/cmd, and a single main key: A-Z, 0-9,
/// F1-F12; punctuation = - , . / ; ' ` [ ] backslash; named keys space,
/// enter/return, tab, backspace, delete/del, escape/esc, insert/ins, home, end,
/// pageup/pgup, pagedown/pgdn, up, down, left, right; and the keypad tokens
/// num0-num9, numadd/keypadadd, numsubtract/keypadsubtract, nummultiply,
/// numdivide, numdecimal, numenter, numequal (punctuation-free by necessity —
/// '+' is the separator). Returns false (and leaves `out` partial) when no main
/// key resolves or the string is empty. At most one main key; extra main keys ->
/// the last one wins.
///
/// Two shifted spellings are accepted as INPUT and normalise to {shift, base key}
/// on a US/ANSI layout, because that is the physical keystroke ImGui reports:
/// '+' / "plus" -> Shift+'=' and '_' / "underscore" -> Shift+'-'. So "Ctrl++"
/// parses identically to "Ctrl+Shift+=" (and the two correctly collide in a
/// conflict check). A trailing '+' is the only position where '+' reads as a key;
/// everywhere else it stays the separator, so "Ctrl++B" is still Ctrl+B.
bool ParseImGuiHotkey(const std::string& spec, ImGuiBugHotkey& out);

/// Inverse of ParseImGuiHotkey: render a canonical "Ctrl+Shift+B" string.
/// Modifiers emitted in fixed order (Ctrl, Shift, Alt, Super) then the main key.
/// Returns "" when hk.key is ImGuiKey_None. Round-trips with ParseImGuiHotkey for
/// every key the parser recognises (letters upper-cased: 'b' -> "B").
///
/// The shifted input spellings are deliberately NOT round-tripped verbatim: the
/// canonical rendering of {ctrl, shift, Equal} is "Ctrl+Shift+=", not "Ctrl++".
/// Store canonical strings (this function's output) in config, so the round-trip
/// stays a fixed point.
std::string StringifyImGuiHotkey(const ImGuiBugHotkey& hk);

/// True when the current ImGui frame matches `hk`: modifier state equals the
/// spec exactly and the main key was just pressed (no auto-repeat).
bool MatchHotkey(const ImGuiIO& io, const ImGuiBugHotkey& hk);

/// True when two parsed combos are the same keystroke (all four modifiers + the
/// main key). Comparing parsed structs rather than spec strings is what makes
/// "Ctrl++" and "Ctrl+Shift+=" collide as they should.
bool SameCombo(const ImGuiBugHotkey& a, const ImGuiBugHotkey& b);

/// Pure conflict check (no ImGui-IO dependency): returns the index of the first
/// entry in `existing` whose modifiers + key equal `candidate`, or -1 if none.
/// Used by the keybinding editor to warn on a duplicate combo.
int FindShortcutConflict(const std::vector<ImGuiBugHotkey>& existing, const ImGuiBugHotkey& candidate);

/// Every ImGuiKey StringifyImGuiHotkey can render, in scan order (letters, digits,
/// F1-F12, then the punctuation / nav-edit / keypad table). ImGuiKey_Escape is
/// deliberately excluded — it is the capture widget's cancel key. The capture
/// widget scans this so a captured combo always survives Stringify -> Parse;
/// deriving it here keeps the capture set from drifting out of sync with the
/// grammar. Built once into a function-local static.
const std::vector<ImGuiKey>& BindableImGuiKeys();

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_IMGUI_HOTKEY_H
