#include "SmatchetHotkeyCapture.h"

#include "ConfigManager.h"        // TrackerConfig (mutated in Draw)
#include "SmatchetLocalization.h" // SmatchetLocalization::T

#include "imgui.h"

#include <string>
#include <vector>

namespace smatchet {
namespace ui {

namespace {

// Scan the grammar's own bindable-key set (ImGuiHotkey.h BindableImGuiKeys) rather
// than a hand-written list here: every key it yields round-trips Stringify -> Parse,
// so a captured combo can always be stored and re-read. Deriving the set from the
// grammar is what lets '=', '-' and the keypad become capturable the moment the
// grammar learns them — a local copy would have silently kept rejecting them.
ImGuiKey FirstBindableKeyPressedThisFrame() {
    const std::vector<ImGuiKey>& bindable = BindableImGuiKeys();
    for (std::size_t i = 0; i < bindable.size(); ++i) {
        if (ImGui::IsKeyPressed(bindable[i], /*repeat*/ false)) {
            return bindable[i];
        }
    }
    return ImGuiKey_None;
}

// Frame stamp of the most recent frame any rebind control spent in capture mode.
int g_hotkeyCaptureArmedFrame = -10;

// A combo whose keys plain typing can produce would fire mid-sentence (bind "K" and
// every k in a comment triggers the command). Shift does NOT count as a modifier here:
// Shift+letter types a capital, and dispatch (SmatchetUI::dispatchKeybindings) treats
// Shift-only combos as typing-unsafe for the same reason — capture and dispatch must
// agree or a combo captures cleanly yet never fires in editors. F1-F12 are allowed
// bare because they never collide with text entry.
bool HotkeyNeedsModifier(const ImGuiBugHotkey& hk) {
    if (hk.ctrl || hk.alt || hk.super) {
        return false;
    }
    return !(hk.key >= ImGuiKey_F1 && hk.key <= ImGuiKey_F12);
}

} // namespace

bool HotkeyCaptureArmedRecently() { return ImGui::GetFrameCount() - g_hotkeyCaptureArmedFrame <= 1; }

bool CaptureImGuiHotkeyThisFrame(ImGuiBugHotkey& out) {
    const ImGuiKey pressed = FirstBindableKeyPressedThisFrame();
    if (pressed == ImGuiKey_None) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    out = ImGuiBugHotkey{};
    out.ctrl = io.KeyCtrl;
    out.shift = io.KeyShift;
    out.alt = io.KeyAlt;
    out.super = io.KeySuper;
    out.key = pressed;
    return true;
}

bool DrawHotkeyRebindControl(const char* idSuffix, const std::string& display,
                             bool& capturing, std::string& out, const char* armLabel) {
    bool committed = false;
    // Function-scoped (one rebind control captures at a time): the arm click below must
    // clear a warning left over from a capture that ended OUTSIDE this function (tab
    // switch clearing the caller's `capturing`, or the quick-bind popup reopening).
    static bool s_showNeedsModifierWarning = false;
    if (!capturing) {
        // armLabel non-null: the label IS the arm button (combo chip / "+ Add"). Null:
        // the original display-text + "Click to rebind" pair, whose button label and id
        // must stay byte-identical — bucket-E addresses it by that exact string.
        if (armLabel == nullptr) {
            if (display.empty()) {
                ImGui::TextDisabled("%s", SmatchetLocalization::T("keybindings.editor.unbound", "(unbound)"));
            } else {
                ImGui::TextUnformatted(display.c_str());
            }
            ImGui::SameLine();
        }
        const std::string btnId =
            std::string(armLabel != nullptr
                            ? armLabel
                            : SmatchetLocalization::T("keybindings.editor.rebindButton", "Click to rebind")) +
            "##rebind" + (idSuffix != nullptr ? idSuffix : "");
        if (ImGui::SmallButton(btnId.c_str())) {
            capturing = true;
            s_showNeedsModifierWarning = false; // fresh capture, fresh slate
        }
    } else {
        g_hotkeyCaptureArmedFrame = ImGui::GetFrameCount();
        ImGui::TextColored(
            ImVec4(0.95f, 0.85f, 0.30f, 1.0f), "%s",
            SmatchetLocalization::T("keybindings.editor.capturing", "Press a key combo... (Esc to cancel)"));
        if (s_showNeedsModifierWarning) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                SmatchetLocalization::T("keybindings.editor.needs_modifier",
                                        "Include Ctrl, Alt, or Win (Shift alone types text) - or an F-key alone."));
        }
        // Esc cancels without clobbering the existing combo; otherwise commit on the
        // first bindable key press.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat*/ false)) {
            capturing = false;
            s_showNeedsModifierWarning = false;
        } else {
            ImGuiBugHotkey hk;
            const bool captured = CaptureImGuiHotkeyThisFrame(hk);
            if (captured && HotkeyNeedsModifier(hk)) {
                s_showNeedsModifierWarning = true; // reject; stay capturing (P2-M4)
            } else if (captured) {
                s_showNeedsModifierWarning = false;
                out = StringifyImGuiHotkey(hk);
                capturing = false;
                committed = true;
            }
        }
    }
    return committed;
}

std::string FindKeybindingConflict(const std::vector<Keybinding>& bindings,
                                   const std::string& candidateHotkey,
                                   const std::string& selfCommandId,
                                   const std::string& selfArgsJson) {
    ImGuiBugHotkey cand;
    if (!ParseImGuiHotkey(candidateHotkey, cand)) {
        return std::string(); // unparseable candidate -> nothing to conflict against
    }
    for (const Keybinding& b : bindings) {
        if (b.CommandId == selfCommandId && b.ArgsJson == selfArgsJson) {
            continue; // the row being edited never conflicts with itself
        }
        if (!b.Enabled || b.Hotkey.empty()) {
            continue; // disabled / unbound rows do not dispatch, so they can't collide
        }
        ImGuiBugHotkey existing;
        if (!ParseImGuiHotkey(b.Hotkey, existing)) {
            continue;
        }
        if (existing.key == cand.key && existing.ctrl == cand.ctrl && existing.shift == cand.shift &&
            existing.alt == cand.alt && existing.super == cand.super) {
            return b.CommandId;
        }
    }
    return std::string();
}

void QuickBindPopup::Open(const std::string& commandId, const std::string& displayName,
                          const std::string& argsJson) {
    commandId_ = commandId;
    displayName_ = displayName;
    argsJson_ = argsJson.empty() ? std::string("{}") : argsJson;
    pendingOpen_ = true;
    capturing_ = false;
    draft_.clear();
}

bool QuickBindPopup::Draw(TrackerConfig& cfg) {
    const char* kPopupId = "###QuickBindPopup";
    if (pendingOpen_) {
        // Seed the draft from the action's current binding (empty if unbound).
        const int idx = cfg.Keybindings.FindBindingIndex(commandId_, argsJson_);
        draft_ = (idx >= 0) ? cfg.Keybindings.Bindings[static_cast<std::size_t>(idx)].Hotkey : std::string();
        capturing_ = false;
        ImGui::OpenPopup(kPopupId);
        pendingOpen_ = false;
    }

    bool changed = false;
    // Title carries the target command so the same popup id serves every button.
    const std::string title =
        std::string(SmatchetLocalization::T("keybindings.quickbind.title", "Set shortcut")) + kPopupId;
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(SmatchetLocalization::T("keybindings.quickbind.forCommand", "Shortcut for:"));
        ImGui::SameLine();
        ImGui::TextUnformatted(displayName_.empty() ? commandId_.c_str() : displayName_.c_str());
        ImGui::Separator();

        DrawHotkeyRebindControl("quickbind", draft_, capturing_, draft_);

        // Live conflict warning (warn-only — single-combo policy still lets the user set it).
        const std::string conflict = FindKeybindingConflict(cfg.Keybindings.Bindings, draft_, commandId_, argsJson_);
        if (!conflict.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s %s",
                               SmatchetLocalization::T("keybindings.quickbind.conflict", "Already bound to:"),
                               conflict.c_str());
        }

        ImGui::Separator();
        const bool canSet = !draft_.empty();
        if (!canSet) {
            ImGui::BeginDisabled(true);
        }
        if (ImGui::Button(SmatchetLocalization::T("keybindings.quickbind.set", "Set"))) {
            cfg.Keybindings.SetBindingHotkey(commandId_, argsJson_, draft_);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        if (!canSet) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button(SmatchetLocalization::T("keybindings.quickbind.clear", "Clear"))) {
            if (cfg.Keybindings.RemoveBinding(commandId_, argsJson_)) {
                changed = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(SmatchetLocalization::T("keybindings.quickbind.cancel", "Cancel"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

} // namespace ui
} // namespace smatchet
