// Preferences "Keyboard Shortcuts" tab — the visible editor for the rebindable
// global shortcut table (cfg.Keybindings). Lists every binding with a searchable
// filter, an inline capture-to-rebind control (shared smatchet::ui widget), a
// per-row conflict warning (warn-only, single-combo policy), enable/clear, and a
// reset-to-defaults. A read-only section lists the special-cased system shortcuts
// that stay non-rebindable (Zen chord, palette nav). Mutations mark the dispatch
// cache dirty + arm the debounced ConfigManager::Save via MarkPrefsDirty.
// See docs/plans/active/keyboard-shortcuts-rebindable.md (PR2, item 10).
//
// Localization idiom: this TU uses plain ImGui + explicit SmatchetLocalization::T()
// (NOT the `#define ImGui SmatchetLocalizedImGui` macro the other prefs TUs use) so
// it can include the shared capture header (which pulls imgui.h) without tripping the
// macro-ordering trap, matching SmatchetHotkeyCapture.cpp.

#include "SmatchetPreferencesUi_detail.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Config/KeybindingsConfig.h"
#include "ConfigManager.h"
#include "SmatchetLocalization.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetHotkeyCapture.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Human label for a command id: the registry Summary (verb-first one-liner) when the
// id resolves, else the raw id (pseudo-bindings like "ui.command_palette" are not in
// the registry). FindLocked's pointer is invalidated by a concurrent Register, so the
// Summary is copied into the returned string immediately.
std::string CommandLabel(AppController& app, const std::string& commandId) {
    const smatchet::cmd::Command* c = app.Commands().FindLocked(commandId);
    if (c != nullptr && !c->Summary.empty()) {
        return c->Summary;
    }
    return commandId;
}

std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool ContainsLower(const std::string& haystack, const std::string& needleLower) {
    return ToLowerAscii(haystack).find(needleLower) != std::string::npos;
}

// A row matches when the (already-lowercased) filter is a substring of the action
// label, the command id, or the bound combo. Empty filter matches everything.
bool RowMatchesFilter(const std::string& filterLower, const std::string& label,
                      const std::string& commandId, const std::string& hotkey) {
    if (filterLower.empty()) {
        return true;
    }
    return ContainsLower(label, filterLower) || ContainsLower(commandId, filterLower) ||
           ContainsLower(hotkey, filterLower);
}

void DrawSystemShortcutRow(const char* label, const char* combo) {
    ImGui::Bullet();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", combo);
}

} // namespace

void DrawKeybindingsPreferencesTab(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    const std::string tabLabel =
        std::string(SmatchetLocalization::T("prefs.tab.keybindings", "Keyboard Shortcuts")) +
        "###prefsTabKeybindings";
    if (!ImGui::BeginTabItem(tabLabel.c_str())) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::Keybindings;

    ImGui::TextWrapped("%s", SmatchetLocalization::T(
                                 "keybindings.editor.intro",
                                 "Rebind in-app keyboard shortcuts. Click a shortcut to capture a new "
                                 "key combo (Esc cancels). Changes save automatically."));
    ImGui::Spacing();

    // Persistent across frames (single Preferences window): the search filter and the
    // command key of the row currently capturing a combo (empty = none; at most one).
    static std::string filter;
    static std::string capturingKey;

    char searchBuf[160];
    std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::InputTextWithHint("###kbSearch",
                                 SmatchetLocalization::T("keybindings.editor.searchHint", "Filter shortcuts..."),
                                 searchBuf, sizeof(searchBuf))) {
        filter = searchBuf;
    }
    const std::string filterLower = ToLowerAscii(filter);

    bool mutated = false;

    std::vector<Keybinding>& binds = d.cfg.Keybindings.Bindings;
    const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("###kbTable", 3, tflags, ImVec2(0.0f, 320.0f))) {
        ImGui::TableSetupColumn(SmatchetLocalization::T("keybindings.editor.colCommand", "Action"),
                                ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(SmatchetLocalization::T("keybindings.editor.colShortcut", "Shortcut"),
                                ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn(SmatchetLocalization::T("keybindings.editor.colActions", ""),
                                ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < binds.size(); ++i) {
            Keybinding& b = binds[i];
            const std::string label = CommandLabel(app, b.CommandId);
            if (!RowMatchesFilter(filterLower, label, b.CommandId, b.Hotkey)) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", b.CommandId.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            const std::string rowKey = b.CommandId + "\x1f" + b.ArgsJson;
            bool capturing = (capturingKey == rowKey);
            std::string out = b.Hotkey;
            const bool committed = smatchet::ui::DrawHotkeyRebindControl("kbrow", b.Hotkey, capturing, out);
            if (capturing) {
                capturingKey = rowKey; // this row armed/holds capture
            } else if (capturingKey == rowKey) {
                capturingKey.clear(); // it just cancelled/committed
            }
            if (committed) {
                b.Hotkey = out;
                b.Enabled = true; // committing a combo re-enables a disabled row
                mutated = true;
            }
            if (!b.Hotkey.empty() && b.Enabled) {
                const std::string conflict =
                    smatchet::ui::FindKeybindingConflict(binds, b.Hotkey, b.CommandId, b.ArgsJson);
                if (!conflict.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s %s",
                                       SmatchetLocalization::T("keybindings.editor.conflict", "conflicts with"),
                                       CommandLabel(app, conflict).c_str());
                }
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Checkbox(SmatchetLocalization::T("keybindings.editor.enabled", "On"), &b.Enabled)) {
                mutated = true;
            }
            ImGui::SameLine();
            const bool canClear = !b.Hotkey.empty();
            if (!canClear) {
                ImGui::BeginDisabled(true);
            }
            if (ImGui::SmallButton(SmatchetLocalization::T("keybindings.editor.clear", "Clear"))) {
                b.Hotkey.clear(); // keep the row listed (unbound) so it stays rebindable
                mutated = true;
            }
            if (!canClear) {
                ImGui::EndDisabled();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button(SmatchetLocalization::T("keybindings.editor.resetDefaults", "Reset all to defaults"))) {
        d.cfg.Keybindings = KeybindingsConfig::Defaults();
        capturingKey.clear();
        mutated = true;
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(
            SmatchetLocalization::T("keybindings.editor.systemHeader", "System shortcuts (not rebindable)"))) {
        DrawSystemShortcutRow(SmatchetLocalization::T("keybindings.system.zenToggle", "Toggle Zen mode"),
                              "Ctrl+M, Z");
        DrawSystemShortcutRow(SmatchetLocalization::T("keybindings.system.zenExit", "Exit Zen mode"), "Esc Esc");
        DrawSystemShortcutRow(
            SmatchetLocalization::T("keybindings.system.paletteNav", "Command palette navigation"),
            "Up / Down / Enter / Esc");
    }

    if (mutated) {
        ui.MarkKeybindingsDirty();
        MarkPrefsDirty(d);
    }

    ImGui::EndTabItem();
}
