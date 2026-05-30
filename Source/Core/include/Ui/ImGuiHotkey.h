#ifndef SMATCHET_UI_IMGUI_HOTKEY_H
#define SMATCHET_UI_IMGUI_HOTKEY_H

#include <string>

#include "imgui.h"

// ImGuiHotkey — parse a human hotkey string ("Ctrl+Shift+B") into modifier flags
// + an ImGuiKey, and match it against the current ImGui input frame. Used by the
// in-app "Log a Bug" hotkey (config: BugReportHotkey). NOT the Whisper Win32-VK
// HotkeyParse (that's a SMATCHET_WITH_WHISPER plugin using OS-global registration).
// docs/plans/active/log-a-bug-github.md Slice 4.

namespace smatchet {
namespace ui {

struct ImGuiBugHotkey {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    ImGuiKey key = ImGuiKey_None;
};

/// Parse "Ctrl+Shift+B" (case-insensitive, '+'-separated). Recognises modifiers
/// ctrl/control, shift, alt, and a single main key: A-Z, 0-9, F1-F12. Returns
/// false (and leaves `out` partial) when no main key resolves or the string is
/// empty. At most one main key; extra main keys -> the last one wins.
bool ParseImGuiHotkey(const std::string& spec, ImGuiBugHotkey& out);

/// True when the current ImGui frame matches `hk`: modifier state equals the
/// spec exactly and the main key was just pressed (no auto-repeat).
bool MatchHotkey(const ImGuiIO& io, const ImGuiBugHotkey& hk);

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_IMGUI_HOTKEY_H
