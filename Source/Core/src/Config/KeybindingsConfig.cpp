// Keybinding-table serialization + the built-in default shortcut set. The defaults
// reproduce the shortcuts that were hardcoded across SmatchetUI / CommandPaletteUi /
// the bug-report poll before the rebindable registry landed (PR1 migration). See
// docs/plans/active/keyboard-shortcuts-rebindable.md.

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
                LOG_WARN("KeybindingsConfig::from_json: skipping malformed keybinding: %s",
                         ex.what());
            }
        }
    }
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
    return c;
}
