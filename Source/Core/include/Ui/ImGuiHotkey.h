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
/// F1-F12; punctuation = - , . / ; ' ` [ ] backslash; and named keys space,
/// enter/return, tab, backspace, delete/del, escape/esc, insert/ins, home, end,
/// pageup/pgup, pagedown/pgdn, up, down, left, right. Returns false (and leaves
/// `out` partial) when no main key resolves or the string is empty. At most one
/// main key; extra main keys -> the last one wins.
bool ParseImGuiHotkey(const std::string& spec, ImGuiBugHotkey& out);

/// Inverse of ParseImGuiHotkey: render a canonical "Ctrl+Shift+B" string.
/// Modifiers emitted in fixed order (Ctrl, Shift, Alt, Super) then the main key.
/// Returns "" when hk.key is ImGuiKey_None. Round-trips with ParseImGuiHotkey for
/// every key the parser recognises (letters upper-cased: 'b' -> "B").
std::string StringifyImGuiHotkey(const ImGuiBugHotkey& hk);

/// True when the current ImGui frame matches `hk`: modifier state equals the
/// spec exactly and the main key was just pressed (no auto-repeat).
bool MatchHotkey(const ImGuiIO& io, const ImGuiBugHotkey& hk);

/// Pure conflict check (no ImGui-IO dependency): returns the index of the first
/// entry in `existing` whose modifiers + key equal `candidate`, or -1 if none.
/// Used by the keybinding editor to warn on a duplicate combo.
int FindShortcutConflict(const std::vector<ImGuiBugHotkey>& existing, const ImGuiBugHotkey& candidate);

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_IMGUI_HOTKEY_H
