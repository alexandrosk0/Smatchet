// Keybinding-table serialization + the built-in default shortcut set. The defaults
// reproduce the shortcuts that were hardcoded across SmatchetUI / CommandPaletteUi /
// the bug-report poll before the rebindable registry landed. See
// docs/plans/shipped/keyboard-shortcuts-rebindable.md.

#include "KeybindingsConfig.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

namespace {

Keybinding MakeBinding(const char* hotkey, const char* commandId, const char* argsJson) {
    Keybinding b;
    b.Hotkey = hotkey;
    b.CommandId = commandId;
    b.ArgsJson = argsJson;
    b.Enabled = true;
    return b;
}

} // namespace

void to_json(nlohmann::json& j, const Keybinding& b) {
    j = nlohmann::json::object();
    j["command_id"] = b.CommandId;
    j["hotkey"] = b.Hotkey;
    j["args_json"] = b.ArgsJson;
    j["enabled"] = b.Enabled;
}

void from_json(const nlohmann::json& j, Keybinding& b) {
    b.CommandId = j.value("command_id", std::string());
    b.Hotkey = j.value("hotkey", std::string());
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

std::string BoundHotkeyDisplay(const std::vector<Keybinding>& bindings, const std::string& commandId) {
    std::string fallback;
    for (const Keybinding& b : bindings) {
        if (b.CommandId != commandId || !b.Enabled || b.Hotkey.empty()) {
            continue;
        }
        if (b.ArgsJson == "{}" || b.ArgsJson.empty()) {
            return b.Hotkey; // exact default-args match wins
        }
        if (fallback.empty()) {
            fallback = b.Hotkey;
        }
    }
    return fallback;
}

std::string BoundHotkeyDisplayForArgs(const std::vector<Keybinding>& bindings, const std::string& commandId,
                                      const std::string& argsJson) {
    // Semantic (order-independent) args comparison via nlohmann ==. Parse with the
    // non-throwing overload + is_discarded() — this is the strict Config zone where
    // an empty catch is a CRITICAL finding, so no try/catch around json::parse.
    // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=keybinding args are app-serialised local config bytes loaded via the bounded config reader, not external ingress; owner=security-audit; revisit=2026-12-31)
    nlohmann::json want = nlohmann::json::parse(argsJson.empty() ? "{}" : argsJson, nullptr, false);
    if (want.is_discarded()) {
        want = nlohmann::json::object();
    }
    for (const Keybinding& b : bindings) {
        if (b.CommandId != commandId || !b.Enabled || b.Hotkey.empty()) {
            continue;
        }
        // SMATCHET_DEVIATION(rule=bare-json-parse-untrusted; reason=keybinding args are app-serialised local config bytes loaded via the bounded config reader, not external ingress; owner=security-audit; revisit=2026-12-31)
        nlohmann::json have = nlohmann::json::parse(b.ArgsJson.empty() ? "{}" : b.ArgsJson, nullptr, false);
        if (have.is_discarded()) {
            have = nlohmann::json::object();
        }
        if (have == want) {
            return b.Hotkey;
        }
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
    if (idx >= 0) {
        Bindings[static_cast<std::size_t>(idx)].Hotkey = hotkey;
        Bindings[static_cast<std::size_t>(idx)].Enabled = true;
        return idx;
    }
    Keybinding b;
    b.CommandId = commandId;
    b.ArgsJson = argsJson;
    b.Hotkey = hotkey;
    b.Enabled = true;
    Bindings.push_back(b);
    return static_cast<int>(Bindings.size()) - 1;
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
    c.Bindings.push_back(MakeBinding("Ctrl+=", "ui.zoom.in", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+-", "ui.zoom.out", "{}"));
    c.Bindings.push_back(MakeBinding("Ctrl+0", "ui.zoom.reset", "{}"));
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
    return c;
}
