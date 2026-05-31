#ifndef SMATCHET_CONFIG_TOOLBAR_CONFIG_H
#define SMATCHET_CONFIG_TOOLBAR_CONFIG_H

// Data model for the customizable icon toolbar (Total-Commander-style button bar)
// rendered below the main menu bar. See docs/plans/shipped/customizable-icon-toolbar.md.
// Header-cost discipline (mirrors ConfigManager.h): this header pulls only
// <nlohmann/json_fwd.hpp>. Button command arguments are stored as a JSON *string*
// (ArgsJson), not a live nlohmann::json member, so no TU including this header pays
// the full json.hpp parse cost. Friend to_json/from_json bodies live in
// Source/Core/src/Config/ToolbarConfig.cpp and are found via ADL.

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

/** What a toolbar button does when clicked. */
enum class ToolbarButtonKind {
    Command = 0,  // dispatch CommandId with ArgsJson via the command registry
    Lua = 1,      // run LuaCode via AppController::ExecuteLuaConsoleSnippet (gated)
    Separator = 2 // visual divider; no action
};

/** One toolbar button. Icons are Font Awesome only. */
struct ToolbarButton {
    ToolbarButtonKind Kind = ToolbarButtonKind::Command;
    std::string IconName;        // stable FA token, e.g. "magnifying-glass" (picker / JSON readability)
    std::string IconGlyph;       // resolved UTF-8 glyph; when empty the render layer resolves from IconName
    std::string Tooltip;         // hover text
    std::string CommandId;       // Kind == Command
    std::string ArgsJson = "{}"; // Kind == Command — command args as JSON text; parsed at dispatch
    std::string LuaCode;         // Kind == Lua

    // Bodies in ToolbarConfig.cpp — declaration suffices for ADL at call sites.
    friend void to_json(nlohmann::json& j, const ToolbarButton& b);
    friend void from_json(const nlohmann::json& j, ToolbarButton& b);
};

/** The global toolbar (per-tracker append lists live in ViewWorkspaceState). */
struct ToolbarConfig {
    bool Visible = true;
    std::vector<ToolbarButton> Buttons;

    /** Curated starter set seeded into fresh configs. */
    static ToolbarConfig Default();

    friend void to_json(nlohmann::json& j, const ToolbarConfig& c);
    friend void from_json(const nlohmann::json& j, ToolbarConfig& c);
};

// Pure effective-toolbar resolution: the global button list, plus — when the current
// tracker has appended buttons — a separator followed by that append list. Adjacent
// separators are collapsed and any leading/trailing separator is trimmed, so the bar
// never shows doubled or dangling dividers. Unit-tested; no ImGui / IO dependency.
std::vector<ToolbarButton> ResolveEffectiveToolbar(const ToolbarConfig& global,
                                                   const std::vector<ToolbarButton>& trackerAppend);

#endif // SMATCHET_CONFIG_TOOLBAR_CONFIG_H
