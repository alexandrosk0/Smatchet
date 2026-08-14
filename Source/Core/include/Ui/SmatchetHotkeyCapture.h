#ifndef SMATCHET_UI_SMATCHET_HOTKEY_CAPTURE_H
#define SMATCHET_UI_SMATCHET_HOTKEY_CAPTURE_H

// Shared in-app ImGuiKey capture widget + the toolbar/command-palette "Set
// shortcut..." quick-bind popup for the rebindable keyboard-shortcut feature.
// Generalises the Whisper push-to-talk rebind UX (SmatchetPreferencesUi_Whisper.cpp)
// but captures an ImGuiKey combo, not a Win32 VK — the in-app shortcuts dispatch on
// ImGui input frames, the Whisper hotkey is an OS-global RegisterHotKey. See
// docs/plans/shipped/keyboard-shortcuts-rebindable.md.

#include <string>
#include <vector>

#include "KeybindingsConfig.h" // Keybinding
#include "Ui/ImGuiHotkey.h"    // ImGuiBugHotkey

struct TrackerConfig; // full definition only needed in the .cpp (mutates Keybindings)

namespace smatchet {
namespace ui {

/// Scan the current ImGui input frame for the first *bindable* non-modifier key
/// press — the set is BindableImGuiKeys() (Ui/ImGuiHotkey.h), i.e. exactly the keys
/// StringifyImGuiHotkey can render, so a captured combo always round-trips through
/// Parse/Stringify. On a hit, snapshot the live modifier state (Ctrl/Shift/Alt/
/// Super) into `out` and return true. Modifier-only / no-key frames return false —
/// a combo is not committed until a real key lands. No auto-repeat.
bool CaptureImGuiHotkeyThisFrame(ImGuiBugHotkey& out);

/// Reusable inline rebind control (shared by the Keyboard Shortcuts editor + the
/// quick-bind popup). Not capturing: shows `display` (or a disabled placeholder
/// when empty) + a "Click to rebind" SmallButton. While capturing: shows the
/// yellow prompt, cancels on Esc, and commits on the first bindable key — writing
/// the canonical combo string to `out`. `capturing` is caller-owned persistent
/// state; `idSuffix` disambiguates the button id when several controls share a
/// frame. Returns true only on the commit frame.
///
/// `armLabel` picks the not-capturing rendering. Null (the default) keeps the
/// display-text + "Click to rebind" pair. Non-null renders a single SmallButton
/// carrying that label as the whole arm affordance — how the editor draws each
/// combo chip (the chip's own combo string is the label, so clicking it rebinds
/// that slot in place) and its "+ Add" button.
bool DrawHotkeyRebindControl(const char* idSuffix, const std::string& display,
                             bool& capturing, std::string& out, const char* armLabel = nullptr);

/// True when any rebind control (editor row / quick-bind popup) was in capture mode
/// this frame or the previous one. dispatchKeybindings consults this to stand down
/// while a capture is armed — otherwise pressing a combo DURING capture also fires
/// the command it is currently bound to on that same frame (P2-M4). The one-frame
/// grace covers draw order (the capture widget draws after dispatch runs).
bool HotkeyCaptureArmedRecently();

/// First binding in `bindings` whose parsed combo equals `candidateHotkey`'s,
/// excluding the (selfCommandId, selfArgsJson) row being edited and any disabled /
/// empty / unparseable binding. Returns the conflicting binding's CommandId, or ""
/// when the combo is free. Shared conflict check for the editor + quick-bind popup.
std::string FindKeybindingConflict(const std::vector<Keybinding>& bindings,
                                   const std::string& candidateHotkey,
                                   const std::string& selfCommandId,
                                   const std::string& selfArgsJson);

/// Small modal owned by SmatchetUI. The toolbar / command-palette "Set shortcut..."
/// context action calls Open() with a target command id; SmatchetUI calls Draw()
/// once per frame. On a commit / clear, Draw mutates `cfg.Keybindings` and returns
/// true so the caller persists the config + marks the dispatch cache dirty.
class QuickBindPopup {
public:
    /// Queue the popup to open on the next Draw, targeting the (commandId, argsJson)
    /// action with a human label for the header. Replaces any still-pending target.
    void Open(const std::string& commandId, const std::string& displayName,
              const std::string& argsJson);

    /// Draw the modal when open/queued. Returns true on the single frame a binding
    /// changed (set or cleared); false otherwise.
    bool Draw(TrackerConfig& cfg);

private:
    bool pendingOpen_ = false;
    bool capturing_ = false;
    std::string commandId_;
    std::string displayName_;
    std::string argsJson_ = "{}";
    std::string draft_; // staged combo string; committed to cfg only on "Set"
};

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_SMATCHET_HOTKEY_CAPTURE_H
