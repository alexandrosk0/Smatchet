#ifndef SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H
#define SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H

// Data model for the rebindable global keyboard-shortcut table. Each binding maps
// a hotkey spec ("Ctrl+Shift+F") to a command id + JSON args, dispatched through the
// unified command registry. See docs/plans/active/keyboard-shortcuts-rebindable.md.
// Header-cost discipline (mirrors ConfigManager.h / ToolbarConfig.h): this header
// pulls only <nlohmann/json_fwd.hpp>. Command arguments are stored as a JSON *string*
// (ArgsJson), parsed at dispatch, so no TU including this header pays the full
// json.hpp parse cost. Friend to_json/from_json bodies live in
// Source/Core/src/Config/KeybindingsConfig.cpp and are found via ADL.

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

/** One rebindable shortcut: a hotkey spec bound to a command + args. */
struct Keybinding {
    std::string CommandId;       // command registry id, e.g. "view.toggle.performance"
    std::string Hotkey;          // human spec, e.g. "Ctrl+Shift+F" (ParseImGuiHotkey grammar)
    std::string ArgsJson = "{}"; // command args as JSON text; parsed at dispatch
    bool Enabled = true;         // disabled bindings are skipped (kept so the editor can re-enable)

    // Bodies in KeybindingsConfig.cpp — declaration suffices for ADL at call sites.
    friend void to_json(nlohmann::json& j, const Keybinding& b);
    friend void from_json(const nlohmann::json& j, Keybinding& b);
};

/** The global keybinding table. */
struct KeybindingsConfig {
    std::vector<Keybinding> Bindings;

    /** Built-in shortcut set seeded into fresh configs (parity with the pre-registry
        hardcoded shortcuts). */
    static KeybindingsConfig Defaults();

    friend void to_json(nlohmann::json& j, const KeybindingsConfig& c);
    friend void from_json(const nlohmann::json& j, KeybindingsConfig& c);
};

#endif // SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H
