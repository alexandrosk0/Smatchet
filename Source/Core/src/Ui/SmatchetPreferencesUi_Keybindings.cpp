// Preferences "Keyboard Shortcuts" tab — the visible editor for the rebindable
// global shortcut table (cfg.Keybindings). Lists every binding with a searchable
// filter, an inline capture-to-rebind control (shared smatchet::ui widget), a
// per-row conflict warning (warn-only, single-combo policy), enable/clear, and a
// reset-to-defaults. A read-only section lists the special-cased system shortcuts
// that stay non-rebindable (Zen chord, palette nav). Mutations mark the dispatch
// cache dirty + arm the debounced ConfigManager::Save via MarkPrefsDirty.
// See docs/plans/shipped/keyboard-shortcuts-rebindable.md (item 10).
// Localization idiom: this TU uses plain ImGui + explicit SmatchetLocalization::T()
// (NOT the `#define ImGui SmatchetLocalizedImGui` macro the other prefs TUs use) so
// it can include the shared capture header (which pulls imgui.h) without tripping the
// macro-ordering trap, matching SmatchetHotkeyCapture.cpp.

#include "SmatchetPreferencesUi_detail.h"

// fan-in Phase 6 T3: this tab only reads the command registry — IAppCommands suffices.
#include "Interfaces/IAppCommands.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Config/KeybindingsConfig.h"
#include "ConfigManager.h"
#include "SmatchetLocalization.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"
#include "Ui/PreferencesFilter.h"
#include "Ui/SmatchetHotkeyCapture.h"

#include "imgui.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Human label for a command id: the registry Summary (verb-first one-liner) when the
// id resolves, else the raw id (pseudo-bindings like "ui.command_palette" are not in
// the registry). FindLocked's pointer is invalidated by a concurrent Register, so the
// Summary is copied into the returned string immediately.
std::string CommandLabel(IAppCommands& app, const std::string& commandId) {
    const smatchet::cmd::Command* c = app.Commands().FindLocked(commandId);
    if (c != nullptr && !c->Summary.empty()) {
        return c->Summary;
    }
    return commandId;
}

// A row matches when the (already-lowercased) filter is a substring of the action
// label, the command id, or the bound combo. Empty filter matches everything.
// The ASCII helpers come from PreferencesFilter — this TU used to carry its own
// copies (Pillar 5).
bool RowMatchesFilter(const std::string& filterLower, const std::string& label, const std::string& commandId,
                      const std::string& hotkey) {
    if (filterLower.empty()) {
        return true;
    }
    return PreferencesFilter::ContainsLower(label, filterLower) ||
           PreferencesFilter::ContainsLower(commandId, filterLower) ||
           PreferencesFilter::ContainsLower(hotkey, filterLower);
}

void DrawSystemShortcutRow(const char* label, const char* combo) {
    ImGui::Bullet();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", combo);
}

// True when any binding row already targets `commandId` (any args). Used to hide
// already-listed commands from the "Add shortcut for a command..." picker.
bool CommandHasRow(const std::vector<Keybinding>& binds, const std::string& commandId) {
    for (const Keybinding& b : binds) {
        if (b.CommandId == commandId) {
            return true;
        }
    }
    return false;
}

// "Add shortcut for a command..." popup: a filterable list of every registry command
// that has no row yet. Picking one appends an unbound+enabled row (the user then
// captures a combo into it via the inline rebind control). Returns true if a row was
// appended so the caller marks the table dirty. Mirrors the toolbar command picker
// (InputTextWithHint filter + scrollable child + Selectable). The popup must be drawn
// every frame (BeginPopup) for OpenPopup to keep it open.
bool DrawAddCommandPopup(IAppCommands& app, std::vector<Keybinding>& binds) {
    // Snapshot the full command registry ONCE per popup-open, not every frame the popup is drawn:
    // App.Commands().All() copies the whole command vector, and BeginPopup returns true on every
    // frame while open. We detect the closed->open transition (prevOpen latch) and refresh the
    // cached snapshot only then; the per-row CommandHasRow(binds) filter still runs live (binds
    // only changes on a pick, which closes the popup). The cache is cleared on close so a reopened
    // popup re-snapshots any commands registered in the interim.
    static std::vector<smatchet::cmd::Command> cachedAll;
    static bool prevOpen = false;
    const bool isOpen = ImGui::IsPopupOpen("###kbAddCommandPopup");
    if (isOpen && !prevOpen) {
        cachedAll = app.Commands().All();
    } else if (!isOpen && prevOpen) {
        cachedAll.clear();
        cachedAll.shrink_to_fit();
    }
    prevOpen = isOpen;

    if (!ImGui::BeginPopup("###kbAddCommandPopup")) {
        return false;
    }
    static std::string addFilter;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s", addFilter.c_str());
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::InputTextWithHint("###kbAddSearch",
                                 SmatchetLocalization::T("keybindings.editor.addSearchHint", "Search commands..."), buf,
                                 sizeof(buf))) {
        addFilter = buf;
    }
    const std::string addFilterLower = PreferencesFilter::ToLowerFold(addFilter);

    bool added = false;
    ImGui::BeginChild("###kbAddList", ImVec2(360.0f, 280.0f), true);
    const std::vector<smatchet::cmd::Command>& all = cachedAll;
    bool anyShown = false;
    for (const smatchet::cmd::Command& c : all) {
        if (CommandHasRow(binds, c.Name)) {
            continue; // already listed in the table
        }
        const std::string label = !c.Summary.empty() ? c.Summary : c.Name;
        if (!addFilterLower.empty() && !PreferencesFilter::ContainsLower(label, addFilterLower) &&
            !PreferencesFilter::ContainsLower(c.Name, addFilterLower)) {
            continue;
        }
        anyShown = true;
        ImGui::PushID(c.Name.c_str());
        if (ImGui::Selectable(label.c_str())) {
            Keybinding nb;
            nb.CommandId = c.Name;
            nb.Hotkey.clear(); // unbound row — user captures the combo inline
            nb.ArgsJson = "{}";
            nb.Enabled = true;
            binds.push_back(nb);
            added = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", c.Name.c_str());
        }
        ImGui::PopID();
        if (added) {
            break; // binds was mutated; stop iterating the now-stale-vs-row list
        }
    }
    if (!anyShown) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("keybindings.editor.addNoneLeft",
                                                          "All commands already have a shortcut row."));
    }
    ImGui::EndChild();
    ImGui::EndPopup();
    return added;
}

// Persistent across frames (single Preferences window): the search filter and the command
// key of the row currently capturing a combo (empty = none; at most one). File-scope so
// ResetKeybindingsCaptureState can abandon an armed capture from drawPreferencesWindow
// when the Keybindings page stops drawing — otherwise the state survives and re-arms
// that row when the page is reopened.
std::string s_kbFilter;
std::string s_kbCapturingKey;

void DrawKeybindingsSectionBody(SmatchetUI& ui, IAppCommands& app, UiDrawSession& d) {
    std::string& filter = s_kbFilter;
    std::string& capturingKey = s_kbCapturingKey;

    // Intro is section chrome — no descriptor row of its own, so it drops out of a
    // narrowed pane.
    const bool globalActive = d.prefsFilter.Active();
    if (!globalActive) {
        ImGui::TextWrapped("%s",
                           SmatchetLocalization::T("keybindings.editor.intro",
                                                   "Rebind in-app keyboard shortcuts. Click a shortcut to capture a "
                                                   "new key combo (Esc cancels). Changes save automatically."));
        ImGui::Spacing();
    }

    // The rows are dynamic command entries with no descriptors of their own, so this
    // table keeps a local filter box. While the global Preferences query is active it
    // takes the row filter over and the local box is hidden — two live filters over
    // one table is a trap.
    if (!globalActive) {
        char searchBuf[160];
        std::snprintf(searchBuf, sizeof(searchBuf), "%s", filter.c_str());
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::InputTextWithHint("###kbSearch",
                                     SmatchetLocalization::T("keybindings.editor.searchHint", "Filter shortcuts..."),
                                     searchBuf, sizeof(searchBuf))) {
            filter = searchBuf;
        }
    }
    std::string filterLower = PreferencesFilter::ToLowerFold(globalActive ? std::string(d.prefsSearchBuf) : filter);

    bool mutated = false;

    std::vector<Keybinding>& binds = d.cfg.Keybindings.Bindings;

    // A global query that reached this section matched the *section* descriptor
    // ("keyboard shortcut hotkey ...") — narrowing the rows by that same word would
    // empty the table. Forward it to the rows only when it actually hits one; the
    // scan already ran this frame in drawPreferencesWindow (the nav rail needs the
    // same answer), so read its result rather than repeating it.
    if (globalActive && !d.prefsKeybindRowsMatchQuery) {
        filterLower.clear();
    }
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
    // P2-M5: this tab autosaves (debounced ~100 ms), so an unconfirmed reset makes the
    // loss of every custom shortcut permanent almost immediately. Route through a confirm.
    if (ImGui::Button(SmatchetLocalization::T("keybindings.editor.resetDefaults", "Reset all to defaults"))) {
        ImGui::OpenPopup("Reset keyboard shortcuts?###KbResetConfirm");
    }
    if (ImGui::BeginPopupModal("Reset keyboard shortcuts?###KbResetConfirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "%s", SmatchetLocalization::Format("keybindings.editor.resetConfirm",
                                               "Replace all %d shortcuts with the defaults? Custom bindings are "
                                               "lost immediately (this tab saves automatically).",
                                               static_cast<int>(d.cfg.Keybindings.Bindings.size())));
        ImGui::Spacing();
        if (ImGui::Button(SmatchetLocalization::T("keybindings.editor.resetConfirmYes", "Reset to defaults"))) {
            d.cfg.Keybindings = KeybindingsConfig::Defaults();
            capturingKey.clear();
            mutated = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(SmatchetLocalization::T("keybindings.editor.resetConfirmNo", "Keep my shortcuts"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::T("keybindings.editor.addCommand", "Add shortcut for a command..."))) {
        ImGui::OpenPopup("###kbAddCommandPopup");
    }
    if (DrawAddCommandPopup(app, binds)) {
        mutated = true;
    }

    if (mutated) {
        ui.MarkKeybindingsDirty();
        MarkPrefsDirty(d);
    }
}

// Shortcuts > System shortcuts body: the fixed, non-rebindable chords. Its own section now —
// the inner CollapsingHeader it used to carry is the PrefsSection header.
void DrawSystemShortcutsSectionBody() {
    DrawSystemShortcutRow(SmatchetLocalization::T("keybindings.system.zenToggle", "Toggle Zen mode"), "Ctrl+M, Z");
    DrawSystemShortcutRow(SmatchetLocalization::T("keybindings.system.zenExit", "Exit Zen mode"), "Esc Esc");
    DrawSystemShortcutRow(SmatchetLocalization::T("keybindings.system.paletteNav", "Command palette navigation"),
                          "Up / Down / Enter / Esc");
}

} // namespace

void DrawKeybindingsPreferencesTab(SmatchetUI& ui, IAppCommands& app, UiDrawSession& d) {
    bool bodyRan = false;
    SmatchetPreferencesUiDetail::PrefsSection(d, "shortcuts.keyboard", [&] {
        // One descriptor covers the whole table (its rows are dynamic command
        // entries); gated at the call site because the body owns capture state.
        if (!d.prefsFilter.ShowSetting("shortcuts.keyboard.bindings")) {
            return;
        }
        bodyRan = true;
        DrawKeybindingsSectionBody(ui, app, d);
    });
    if (!bodyRan) {
        // Section collapsed (or filtered out) mid-capture — drop the armed row, matching
        // the old inactive-tab early-return semantics.
        s_kbCapturingKey.clear();
    }
    SmatchetPreferencesUiDetail::PrefsSection(d, "shortcuts.system", [&] {
        if (d.prefsFilter.ShowSetting("shortcuts.system.list")) {
            DrawSystemShortcutsSectionBody();
        }
    });
}

namespace SmatchetPreferencesUiDetail {
void ResetKeybindingsCaptureState() { s_kbCapturingKey.clear(); }

bool AnyKeybindingRowMatchesQuery(IAppCommands& app, UiDrawSession& d) {
    if (!d.prefsFilter.Active()) {
        return false;
    }
    const std::string filterLower = PreferencesFilter::ToLowerFold(std::string(d.prefsSearchBuf));
    const std::vector<Keybinding>& binds = d.cfg.Keybindings.Bindings;
    for (std::size_t i = 0; i < binds.size(); ++i) {
        if (RowMatchesFilter(filterLower, CommandLabel(app, binds[i].CommandId), binds[i].CommandId, binds[i].Hotkey)) {
            return true;
        }
    }
    return false;
}
} // namespace SmatchetPreferencesUiDetail
