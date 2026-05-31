// Toolbar config serialization, default starter set, and pure effective-toolbar
// resolution. See docs/plans/shipped/customizable-icon-toolbar.md.

#include "ToolbarConfig.h"

#include <nlohmann/json.hpp>

namespace {

ToolbarButton MakeCommandButton(const char* iconName, const char* tooltip, const char* commandId,
                                const char* argsJson) {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Command;
    b.IconName = iconName;
    b.Tooltip = tooltip;
    b.CommandId = commandId;
    b.ArgsJson = argsJson;
    return b;
}

ToolbarButton MakeSeparator() {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Separator;
    return b;
}

} // namespace

void to_json(nlohmann::json& j, const ToolbarButton& b) {
    j = nlohmann::json::object();
    j["kind"] = static_cast<int>(b.Kind);
    j["icon_name"] = b.IconName;
    j["icon_glyph"] = b.IconGlyph;
    j["tooltip"] = b.Tooltip;
    j["command_id"] = b.CommandId;
    j["args_json"] = b.ArgsJson;
    j["lua_code"] = b.LuaCode;
}

void from_json(const nlohmann::json& j, ToolbarButton& b) {
    int kind = j.value("kind", 0);
    if (kind < 0 || kind > 2) {
        kind = 0; // unknown kind → Command (renders disabled if CommandId is unknown)
    }
    b.Kind = static_cast<ToolbarButtonKind>(kind);
    b.IconName = j.value("icon_name", std::string());
    b.IconGlyph = j.value("icon_glyph", std::string());
    b.Tooltip = j.value("tooltip", std::string());
    b.CommandId = j.value("command_id", std::string());
    b.ArgsJson = j.value("args_json", std::string("{}"));
    b.LuaCode = j.value("lua_code", std::string());
}

void to_json(nlohmann::json& j, const ToolbarConfig& c) {
    j = nlohmann::json::object();
    j["visible"] = c.Visible;
    j["buttons"] = c.Buttons;
}

void from_json(const nlohmann::json& j, ToolbarConfig& c) {
    c.Visible = j.value("visible", true);
    c.Buttons.clear();
    if (j.contains("buttons") && j["buttons"].is_array()) {
        for (const auto& item : j["buttons"]) {
            if (!item.is_object()) {
                continue;
            }
            try {
                c.Buttons.push_back(item.get<ToolbarButton>());
            } catch (...) { // catch-all-ok: skip a malformed button, keep the rest
            }
        }
    }
}

ToolbarConfig ToolbarConfig::Default() {
    ToolbarConfig c;
    c.Visible = true;
    // Panel-open actions (palette / new-view / settings / customize) bind to the ui.*
    // thin commands added in a later slice; they render disabled until those land
    // (unknown-command-id guard). The sync/refresh button is intentionally omitted
    // until a concrete sync command id is confirmed (plan § sync-button fallback).
    c.Buttons.push_back(MakeCommandButton("magnifying-glass", "Command palette", "ui.command_palette", "{}"));
    c.Buttons.push_back(MakeCommandButton("list", "Views", "view.list", "{}"));
    c.Buttons.push_back(MakeCommandButton("gear", "Settings", "ui.settings", "{}"));
    c.Buttons.push_back(MakeSeparator());
    c.Buttons.push_back(MakeCommandButton("lock", "Enable read-only", "app.set_readonly", "{\"on\":true}"));
    c.Buttons.push_back(MakeCommandButton("sliders", "Customize toolbar", "ui.toolbar_customize", "{}"));
    return c;
}

std::vector<ToolbarButton> ResolveEffectiveToolbar(const ToolbarConfig& global,
                                                   const std::vector<ToolbarButton>& trackerAppend) {
    std::vector<ToolbarButton> merged = global.Buttons;
    if (!trackerAppend.empty()) {
        ToolbarButton sep;
        sep.Kind = ToolbarButtonKind::Separator;
        merged.push_back(sep);
        merged.insert(merged.end(), trackerAppend.begin(), trackerAppend.end());
    }

    // Collapse adjacent separators + trim any leading separator.
    std::vector<ToolbarButton> cleaned;
    cleaned.reserve(merged.size());
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const bool isSep = merged[i].Kind == ToolbarButtonKind::Separator;
        if (isSep) {
            if (cleaned.empty()) {
                continue; // trim leading separator
            }
            if (cleaned.back().Kind == ToolbarButtonKind::Separator) {
                continue; // collapse adjacent separators
            }
        }
        cleaned.push_back(merged[i]);
    }
    // Trim any trailing separator.
    while (!cleaned.empty() && cleaned.back().Kind == ToolbarButtonKind::Separator) {
        cleaned.pop_back();
    }
    return cleaned;
}
