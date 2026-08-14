// Keybinding-table serialization + the built-in default shortcut set. The defaults
// reproduce the shortcuts that were hardcoded across SmatchetUI / CommandPaletteUi /
// the bug-report poll before the rebindable registry landed. See
// docs/plans/shipped/keyboard-shortcuts-rebindable.md.

#include "KeybindingsConfig.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <initializer_list>

namespace {

Keybinding MakeBindingMulti(std::initializer_list<const char*> hotkeys, const char* commandId, const char* argsJson) {
    Keybinding b;
    for (const char* h : hotkeys) {
        b.AddHotkey(h);
    }
    b.CommandId = commandId;
    b.ArgsJson = argsJson;
    b.Enabled = true;
    return b;
}

// Single-combo convenience — one body, so the multi-combo rows below cannot drift
// from the single-combo ones.
Keybinding MakeBinding(const char* hotkey, const char* commandId, const char* argsJson) {
    return MakeBindingMulti({hotkey}, commandId, argsJson);
}

} // namespace

std::string Keybinding::PrimaryHotkey() const { return Hotkeys.empty() ? std::string() : Hotkeys.front(); }

bool Keybinding::HasHotkey(const std::string& hotkey) const {
    for (std::size_t i = 0; i < Hotkeys.size(); ++i) {
        if (Hotkeys[i] == hotkey) {
            return true;
        }
    }
    return false;
}

bool Keybinding::AddHotkey(const std::string& hotkey) {
    if (hotkey.empty() || HasHotkey(hotkey)) {
        return false;
    }
    Hotkeys.push_back(hotkey);
    return true;
}

bool Keybinding::RemoveHotkey(const std::string& hotkey) {
    for (std::size_t i = 0; i < Hotkeys.size(); ++i) {
        if (Hotkeys[i] == hotkey) {
            Hotkeys.erase(Hotkeys.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void to_json(nlohmann::json& j, const Keybinding& b) {
    j = nlohmann::json::object();
    j["command_id"] = b.CommandId;
    // Legacy single-combo field, still written for DOWNGRADE safety: one config file
    // can be shared by builds of different ages (standalone vs Unreal-embedded), and
    // a build that predates the alias list reads this and gets a working primary
    // combo instead of an unbound action. Redundant with hotkeys[0]; the reader
    // de-dups. Accepted caveat: an OLD build that saves drops the hotkeys array.
    j["hotkey"] = b.PrimaryHotkey();
    j["hotkeys"] = b.Hotkeys;
    j["args_json"] = b.ArgsJson;
    j["enabled"] = b.Enabled;
}

void from_json(const nlohmann::json& j, Keybinding& b) {
    b.CommandId = j.value("command_id", std::string());
    b.Hotkeys.clear();
    if (j.contains("hotkeys") && j["hotkeys"].is_array()) {
        for (const auto& h : j["hotkeys"]) {
            if (!h.is_string()) {
                continue; // tolerate a hand-edited stray; keep the rest of the set
            }
            b.AddHotkey(h.get<std::string>()); // skips empty + duplicate
        }
    }
    // The legacy single-combo field is read AFTER the list, so a config written by a
    // current build round-trips to a no-op (hotkey == hotkeys[0], de-duped by
    // AddHotkey) while a pre-alias config (hotkey only) yields a one-combo set. A
    // wrong JSON type here still throws, and the caller's try/catch skips just this
    // binding — that malformed-entry contract is unchanged.
    b.AddHotkey(j.value("hotkey", std::string()));
    b.ArgsJson = j.value("args_json", std::string("{}"));
    b.Enabled = j.value("enabled", true);
}

void to_json(nlohmann::json& j, const KeybindingsConfig& c) {
    j = nlohmann::json::object();
    j["bindings"] = c.Bindings;
}

void from_json(const nlohmann::json& j, KeybindingsConfig& c) {
    c.Bindings.clear();
    if (j.contains("bindings") && j["bindings"].is_array()) {
        for (const auto& item : j["bindings"]) {
            if (!item.is_object()) {
                continue;
            }
            try {
                c.Bindings.push_back(item.get<Keybinding>());
            } catch (const std::exception& ex) {
                // Skip a malformed binding, keep the rest of the table.
                LOG_WARN("KeybindingsConfig::from_json: skipping malformed keybinding: %s", ex.what());
            }
        }
    }
}

namespace {

// The one binding a display surface should render for an action. `matchArgs` picks
// the rule: false = command-id only, preferring the default-args ("{}") row and
// falling back to any (BoundHotkeyDisplay); true = semantic, order-independent args
// equality, needed when one command id carries several distinct-args bindings on
// distinct keys (BoundHotkeyDisplayForArgs / ...All). Sharing one body keeps the
// json-parse deviation below in a single place instead of once per display helper.
const Keybinding* FindDisplayBinding(const std::vector<Keybinding>& bindings, const std::string& commandId,
                                     const std::string& argsJson, bool matchArgs) {
    // Semantic (order-independent) args comparison via nlohmann ==. Parse with the
    // non-throwing overload + is_discarded() — this is the strict Config zone where
    // an empty catch is a CRITICAL finding, so no try/catch around json::parse.
    nlohmann::json want;
    if (matchArgs) {
        // clang-format off
        // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=keybinding args are app-serialised local config bytes loaded via the bounded config reader, not external ingress; owner=security-audit; revisit=2026-12-31)
        want = nlohmann::json::parse(argsJson.empty() ? "{}" : argsJson, nullptr, false);
        // clang-format on
        if (want.is_discarded()) {
            want = nlohmann::json::object();
        }
    }
    const Keybinding* fallback = nullptr;
    for (const Keybinding& b : bindings) {
        if (b.CommandId != commandId || !b.Enabled || b.Hotkeys.empty()) {
            continue;
        }
        if (!matchArgs) {
            if (b.ArgsJson == "{}" || b.ArgsJson.empty()) {
                return &b; // exact default-args match wins
            }
            if (fallback == nullptr) {
                fallback = &b;
            }
            continue;
        }
        // clang-format off
        // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=keybinding args are app-serialised local config bytes loaded via the bounded config reader, not external ingress; owner=security-audit; revisit=2026-12-31)
        nlohmann::json have = nlohmann::json::parse(b.ArgsJson.empty() ? "{}" : b.ArgsJson, nullptr, false);
        // clang-format on
        if (have.is_discarded()) {
            have = nlohmann::json::object();
        }
        if (have == want) {
            return &b;
        }
    }
    return fallback;
}

} // namespace

std::string BoundHotkeyDisplay(const std::vector<Keybinding>& bindings, const std::string& commandId) {
    const Keybinding* b = FindDisplayBinding(bindings, commandId, "{}", /*matchArgs*/ false);
    return b != nullptr ? b->PrimaryHotkey() : std::string();
}

std::string BoundHotkeyDisplayAll(const std::vector<Keybinding>& bindings, const std::string& commandId,
                                  const std::string& argsJson, const char* sep) {
    const Keybinding* b = FindDisplayBinding(bindings, commandId, argsJson, /*matchArgs*/ true);
    if (b == nullptr) {
        return std::string();
    }
    std::string out;
    for (std::size_t i = 0; i < b->Hotkeys.size(); ++i) {
        if (i != 0U) {
            out += (sep != nullptr ? sep : " / ");
        }
        out += b->Hotkeys[i];
    }
    return out;
}

std::string BoundHotkeyDisplayForArgs(const std::vector<Keybinding>& bindings, const std::string& commandId,
                                      const std::string& argsJson) {
    const Keybinding* b = FindDisplayBinding(bindings, commandId, argsJson, /*matchArgs*/ true);
    if (b != nullptr) {
        return b->PrimaryHotkey();
    }
    return std::string();
}

int KeybindingsConfig::FindBindingIndex(const std::string& commandId, const std::string& argsJson) const {
    for (std::size_t i = 0; i < Bindings.size(); ++i) {
        if (Bindings[i].CommandId == commandId && Bindings[i].ArgsJson == argsJson) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int KeybindingsConfig::SetBindingHotkey(const std::string& commandId, const std::string& argsJson,
                                        const std::string& hotkey) {
    int idx = FindBindingIndex(commandId, argsJson);
    if (idx < 0) {
        Keybinding b;
        b.CommandId = commandId;
        b.ArgsJson = argsJson;
        b.Enabled = true;
        Bindings.push_back(b);
        idx = static_cast<int>(Bindings.size()) - 1;
    }
    Keybinding& row = Bindings[static_cast<std::size_t>(idx)];
    // REPLACE-ALL, not append: "this action's shortcut is now X" drops any
    // alternatives (see the header). AddBindingHotkey is the additive door.
    row.Hotkeys.clear();
    row.AddHotkey(hotkey); // no-op for an empty hotkey -> an unbound-but-listed row
    row.Enabled = true;
    return idx;
}

int KeybindingsConfig::AddBindingHotkey(const std::string& commandId, const std::string& argsJson,
                                        const std::string& hotkey) {
    if (hotkey.empty()) {
        return -1;
    }
    int idx = FindBindingIndex(commandId, argsJson);
    if (idx < 0) {
        Keybinding b;
        b.CommandId = commandId;
        b.ArgsJson = argsJson;
        b.Enabled = true;
        Bindings.push_back(b);
        idx = static_cast<int>(Bindings.size()) - 1;
    }
    Keybinding& row = Bindings[static_cast<std::size_t>(idx)];
    row.AddHotkey(hotkey); // dedups
    row.Enabled = true;    // adding a combo re-enables a disabled action
    return idx;
}

bool KeybindingsConfig::RemoveBindingHotkey(const std::string& commandId, const std::string& argsJson,
                                            const std::string& hotkey) {
    const int idx = FindBindingIndex(commandId, argsJson);
    if (idx < 0) {
        return false;
    }
    // The row survives an emptied combo set on purpose: an unbound row stays visible
    // and rebindable in the editor, matching what "Clear" has always done.
    return Bindings[static_cast<std::size_t>(idx)].RemoveHotkey(hotkey);
}

bool KeybindingsConfig::RemoveBinding(const std::string& commandId, const std::string& argsJson) {
    int idx = FindBindingIndex(commandId, argsJson);
    if (idx < 0) {
        return false;
    }
    Bindings.erase(Bindings.begin() + idx);
    return true;
}

KeybindingsConfig KeybindingsConfig::Defaults() {
    KeybindingsConfig c;
    // Panel visibility — toggle persisted cfg slots (was: handlePanelVisibilityShortcuts).
    c.Bindings.push_back(MakeBinding("Ctrl+B", "view.sidebar.primary", "{\"action\":\"toggle\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Alt+B", "view.sidebar.secondary", "{\"action\":\"toggle\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+J", "view.panel.bottom", "{\"action\":\"toggle\"}"));
    // View reveal — open + focus (was: handleViewRevealShortcuts; show, not toggle).
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+A", "view.assistant", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+F", "view.toggle.performance", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+D", "view.toggle.plan_doc_viewer", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+I", "view.toggle.bulk_import", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+X", "view.toggle.bulk_export", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+K", "view.toggle.mcp_server", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+L", "view.toggle.scripts", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+,", "view.toggle.preferences", "{\"action\":\"show\"}"));
    // App-level chrome (was: drawChromeAndModeToggles / F11 / Ctrl+Shift+P / bug poll).
    c.Bindings.push_back(MakeBinding("Ctrl+Alt+D", "app.dock_debug.toggle", "{}"));
    c.Bindings.push_back(MakeBinding("F11", "app.fullscreen.toggle", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+P", "ui.command_palette", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+B", "app.bug_report.open", "{}"));
    // Menu-bar shortcuts that previously had hints but no working binding
    // (docs/plans/keybindings-menu-shortcuts-fix.md). Zoom + open-view +
    // clear-selection are app-global registry commands (BuiltinCommands_Ui.cpp).
    // Zoom carries alias combos because one intent has several physical spellings.
    // "Ctrl and +" is Ctrl+Shift+= on a US layout (the '+' character is the shifted
    // '='), which the exact-modifier matcher treats as a different keystroke from
    // Ctrl+=; the keypad variants are layout-independent and cover the other habit.
    // Specs are the CANONICAL StringifyImGuiHotkey forms — "Ctrl++" parses to the
    // same combo but canonicalises to "Ctrl+Shift+=", so storing it would break the
    // round-trip fixed point.
    c.Bindings.push_back(MakeBindingMulti({"Ctrl+=", "Ctrl+Shift+=", "Ctrl+NumAdd"}, "ui.zoom.in", "{}"));
    c.Bindings.push_back(MakeBindingMulti({"Ctrl+-", "Ctrl+NumSubtract"}, "ui.zoom.out", "{}"));
    c.Bindings.push_back(MakeBindingMulti({"Ctrl+0", "Ctrl+Num0"}, "ui.zoom.reset", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+V", "ui.open_view", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+G", "grid.clear_selection", "{}"));
    // View reveals. "Open Project View" and "Views Dashboard" share the
    // views_dashboard command but bind distinct keys via distinct args (matched
    // semantically by BoundHotkeyDisplayForArgs).
    c.Bindings.push_back(
        MakeBinding("Ctrl+O", "view.toggle.views_dashboard", "{\"action\":\"show\",\"via\":\"open_project_view\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+E", "view.toggle.views_dashboard", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+U", "view.toggle.log", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+M", "view.toggle.backend_audit", "{\"action\":\"show\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+N", "view.toggle.source_annotate", "{\"action\":\"toggle\"}"));
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+Y", "view.toggle.notifications", "{\"action\":\"show\"}"));
    // Quick-create issue popup (docs/plans/quick-create-issue-unreal-context.md). Ctrl+Shift+J is
    // NOT used here on purpose — the Unreal host reserves it for the overlay visibility toggle.
    c.Bindings.push_back(MakeBinding("Ctrl+Shift+T", "issue.quick_create.open", "{}"));
    return c;
}
