#ifndef SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H
#define SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H

// Data model for the rebindable global keyboard-shortcut table. Each binding maps
// a hotkey spec ("Ctrl+Shift+F") to a command id + JSON args, dispatched through the
// unified command registry. See docs/plans/shipped/keyboard-shortcuts-rebindable.md.
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

    /** Index of the first binding for (commandId, argsJson), or -1 if none. The
        (commandId, argsJson) pair is the action key — the same command with
        different args (e.g. sidebar toggle vs show) is a distinct binding. Pure;
        no ImGui dependency, so the editor + the toolbar/palette quick-bind both
        share it and it is bucket-A testable. */
    int FindBindingIndex(const std::string& commandId, const std::string& argsJson) const;

    /** Upsert the hotkey for (commandId, argsJson): mutate the existing binding if
        present (re-enabling it), else append a fresh one. Returns the affected
        index. An empty hotkey is allowed (an "unbound but listed" row). */
    int SetBindingHotkey(const std::string& commandId, const std::string& argsJson, const std::string& hotkey);

    /** Remove the first binding for (commandId, argsJson). Returns true if one was
        erased. */
    bool RemoveBinding(const std::string& commandId, const std::string& argsJson);

    friend void to_json(nlohmann::json& j, const KeybindingsConfig& c);
    friend void from_json(const nlohmann::json& j, KeybindingsConfig& c);
};

/** Display hotkey to surface for a command id on menus / tooltips / palette rows:
    the first enabled, non-empty Hotkey bound to commandId, preferring the
    default-args ("{}") binding, else any. Empty string when unbound. Pure (no
    ImGui) so the palette rows, toolbar tooltips, and menu shortcut hints all share
    one lookup (DRY / Pillar 5) and it is bucket-A testable. */
std::string BoundHotkeyDisplay(const std::vector<Keybinding>& bindings, const std::string& commandId);

/** Args-aware variant of BoundHotkeyDisplay: the hotkey bound to (commandId, args)
    where `argsJson` is matched by JSON *semantic* equality (key order independent),
    not string equality. Needed when one command id carries several distinct-args
    bindings to distinct keys (e.g. view.toggle.views_dashboard for both "Open
    Project View" and "Views Dashboard"). Empty/"{}" argsJson matches a binding
    with empty/"{}" args. Pure; no ImGui. Empty string when unbound. */
std::string BoundHotkeyDisplayForArgs(const std::vector<Keybinding>& bindings, const std::string& commandId,
                                      const std::string& argsJson);

#endif // SMATCHET_CONFIG_KEYBINDINGS_CONFIG_H
