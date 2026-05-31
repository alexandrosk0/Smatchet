// Customizable icon toolbar — render, click dispatch, right-click menus, and the
// Customize Toolbar editor. See docs/plans/active/customizable-icon-toolbar.md.

#include "Ui/SmatchetToolbarUi.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Config/ConfigManager.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "Ui/SmatchetImGuiFonts.h"
#include "Ui/SmatchetUiSession.h"
#include "imgui.h"
#include "imgui_internal.h"  // BeginViewportSideBar (pins the bar below the menu)

namespace {

bool IsUiPseudoCommand(const std::string& id) {
    return id == "ui.command_palette" || id == "ui.settings" || id == "ui.toolbar_customize";
}

std::string GlyphFor(const ToolbarButton& b) {
    return b.IconGlyph.empty() ? SmatchetToolbarIconGlyph(b.IconName) : b.IconGlyph;
}

bool ButtonEnabled(AppController& app, const ToolbarButton& b) {
    if (b.Kind == ToolbarButtonKind::Lua) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        return true;
#else
        return false;
#endif
    }
    if (b.Kind == ToolbarButtonKind::Command) {
        if (b.CommandId.empty()) {
            return false;
        }
        return IsUiPseudoCommand(b.CommandId) || app.Commands().HasExact(b.CommandId);
    }
    return true;  // separators are inert
}

}  // namespace

void SmatchetToolbarUi::OpenEditor() { requestEditorOpen_ = true; }

void SmatchetToolbarUi::Draw(AppController& app, TrackerConfig& cfg) {
    if (cfg.Toolbar.Visible) {
        RenderBar(app, cfg);
    }
    RenderEditor(app, cfg);
}

void SmatchetToolbarUi::DispatchButton(AppController& app, TrackerConfig& cfg, const ToolbarButton& b) {
    if (b.Kind == ToolbarButtonKind::Separator) {
        return;
    }
    if (b.Kind == ToolbarButtonKind::Lua) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        std::string err;
        std::string summary;
        if (!app.ExecuteLuaConsoleSnippet(b.LuaCode, err, summary)) {
            LOG_WARN("Toolbar Lua button failed: %s", err.c_str());
        }
#else
        LOG_WARN("Toolbar Lua button ignored: Lua automation not built in this configuration");
#endif
        return;
    }

    // ui.* pseudo-actions are handled in the UI layer (no registry command).
    if (b.CommandId == "ui.toolbar_customize") {
        requestEditorOpen_ = true;
        return;
    }
    if (b.CommandId == "ui.command_palette") {
        g_ui.requestCommandPaletteOpen = true;
        return;
    }
    if (b.CommandId == "ui.settings") {
        cfg.ShowPreferencesWindow = true;
        return;
    }

    nlohmann::json args = nlohmann::json::object();
    if (!b.ArgsJson.empty()) {
        try {
            args = nlohmann::json::parse(b.ArgsJson);
        } catch (...) {  // catch-all-ok: invalid args JSON → dispatch with empty object
            args = nlohmann::json::object();
        }
    }
    smatchet::cmd::CommandContext ctx;
    ctx.App = &app;
    ctx.Source = smatchet::cmd::CommandSource::Internal;
    const smatchet::cmd::CommandResult r = app.Commands().Dispatch(b.CommandId, args, ctx);
    if (!r.Ok) {
        LOG_WARN("Toolbar command '%s' failed", b.CommandId.c_str());
    }
}

void SmatchetToolbarUi::RenderBar(AppController& app, TrackerConfig& cfg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float barH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginViewportSideBar("##SmatchetToolbar", vp, ImGuiDir_Up, barH, flags)) {
        const std::vector<ToolbarButton> eff = ResolveEffectiveToolbar(cfg.Toolbar, std::vector<ToolbarButton>());
        const bool fa = SmatchetAreFaIconsLoaded();

        for (std::size_t i = 0; i < eff.size(); ++i) {
            const ToolbarButton& b = eff[i];
            if (i != 0) {
                ImGui::SameLine();
            }
            if (b.Kind == ToolbarButtonKind::Separator) {
                ImGui::TextDisabled("|");
                continue;
            }

            std::string glyph = GlyphFor(b);
            std::string label;
            if (fa && !glyph.empty()) {
                label = glyph;
            } else if (!b.Tooltip.empty()) {
                label = b.Tooltip.substr(0, 2);
            } else {
                label = b.CommandId.substr(0, 2);
            }
            label += "##tb";
            label += std::to_string(i);

            const bool enabled = ButtonEnabled(app, b);
            if (!enabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label.c_str())) {
                DispatchButton(app, cfg, b);
            }
            if (!enabled) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tip = b.Tooltip.empty() ? b.CommandId : b.Tooltip;
                if (b.Kind == ToolbarButtonKind::Command && !enabled) {
                    tip += "  (command not found)";
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }

        // Right-click anywhere on the bar → context menu.
        if (ImGui::BeginPopupContextWindow("##ToolbarCtx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("Customize Toolbar...")) {
                requestEditorOpen_ = true;
            }
            if (ImGui::MenuItem("Hide Toolbar")) {
                cfg.Toolbar.Visible = false;
                ConfigManager::Save(cfg);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void SmatchetToolbarUi::RenderEditor(AppController& app, TrackerConfig& cfg) {
    if (requestEditorOpen_) {
        requestEditorOpen_ = false;
        editBuf_ = cfg.Toolbar;
        selected_ = editBuf_.Buttons.empty() ? -1 : 0;
        cmdNames_.clear();
        const std::vector<smatchet::cmd::Command> all = app.Commands().All();
        cmdNames_.reserve(all.size());
        for (const smatchet::cmd::Command& c : all) {
            cmdNames_.push_back(c.Name);
        }
        cmdSearch_[0] = '\0';
        ImGui::OpenPopup("Customize Toolbar##SmatchetToolbarEditor");
    }

    ImGui::SetNextWindowSize(ImVec2(620.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Customize Toolbar##SmatchetToolbarEditor", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const bool fa = SmatchetAreFaIconsLoaded();

    // Action row.
    if (ImGui::Button("Add command")) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Command;
        b.Tooltip = "New button";
        editBuf_.Buttons.push_back(b);
        selected_ = static_cast<int>(editBuf_.Buttons.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Lua")) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Lua;
        b.Tooltip = "New Lua button";
        editBuf_.Buttons.push_back(b);
        selected_ = static_cast<int>(editBuf_.Buttons.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add separator")) {
        ToolbarButton b;
        b.Kind = ToolbarButtonKind::Separator;
        editBuf_.Buttons.push_back(b);
        selected_ = static_cast<int>(editBuf_.Buttons.size()) - 1;
    }

    const int count = static_cast<int>(editBuf_.Buttons.size());
    const bool hasSel = selected_ >= 0 && selected_ < count;

    ImGui::SameLine();
    if (!hasSel) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Duplicate") && hasSel) {
        editBuf_.Buttons.insert(editBuf_.Buttons.begin() + selected_ + 1, editBuf_.Buttons[selected_]);
        ++selected_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && hasSel) {
        editBuf_.Buttons.erase(editBuf_.Buttons.begin() + selected_);
        selected_ = editBuf_.Buttons.empty() ? -1 : (std::min)(selected_, static_cast<int>(editBuf_.Buttons.size()) - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Up") && hasSel && selected_ > 0) {
        std::swap(editBuf_.Buttons[selected_], editBuf_.Buttons[selected_ - 1]);
        --selected_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Down") && hasSel && selected_ < count - 1) {
        std::swap(editBuf_.Buttons[selected_], editBuf_.Buttons[selected_ + 1]);
        ++selected_;
    }
    if (!hasSel) {
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // Left: button list (drag-drop reorder). Right: field editor.
    ImGui::BeginChild("##tblist", ImVec2(240.0f, -ImGui::GetFrameHeightWithSpacing()), true);
    for (int i = 0; i < static_cast<int>(editBuf_.Buttons.size()); ++i) {
        ToolbarButton& b = editBuf_.Buttons[i];
        std::string rowLabel;
        if (b.Kind == ToolbarButtonKind::Separator) {
            rowLabel = "--- separator ---";
        } else {
            const std::string g = GlyphFor(b);
            rowLabel = (fa && !g.empty()) ? (g + "  ") : std::string("[ ]  ");
            rowLabel += b.Tooltip.empty() ? (b.CommandId.empty() ? std::string("(unset)") : b.CommandId) : b.Tooltip;
        }
        ImGui::PushID(i);
        if (ImGui::Selectable(rowLabel.c_str(), selected_ == i)) {
            selected_ = i;
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("SMATCHET_TB_BTN", &i, sizeof(int));
            ImGui::TextUnformatted(rowLabel.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SMATCHET_TB_BTN")) {
                const int src = *static_cast<const int*>(p->Data);
                if (src >= 0 && src < static_cast<int>(editBuf_.Buttons.size()) && src != i) {
                    ToolbarButton moved = editBuf_.Buttons[src];
                    editBuf_.Buttons.erase(editBuf_.Buttons.begin() + src);
                    int dst = (src < i) ? (i - 1) : i;
                    editBuf_.Buttons.insert(editBuf_.Buttons.begin() + dst, moved);
                    selected_ = dst;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##tbedit", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);
    if (selected_ >= 0 && selected_ < static_cast<int>(editBuf_.Buttons.size())) {
        ToolbarButton& b = editBuf_.Buttons[selected_];
        const char* kinds[] = {"Command", "Lua", "Separator"};
        int k = static_cast<int>(b.Kind);
        if (ImGui::Combo("Kind", &k, kinds, 3)) {
            b.Kind = static_cast<ToolbarButtonKind>(k);
        }

        if (b.Kind != ToolbarButtonKind::Separator) {
            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", b.IconName.c_str());
            if (ImGui::InputText("Icon name", nameBuf, sizeof(nameBuf))) {
                b.IconName = nameBuf;
                b.IconGlyph = SmatchetToolbarIconGlyph(b.IconName);
            }
            ImGui::SameLine();
            if (ImGui::Button("Pick...")) {
                iconPicker_.Open();
            }
            {
                std::string pn;
                std::string pg;
                if (iconPicker_.Draw(pn, pg)) {
                    b.IconName = pn;
                    b.IconGlyph = pg;
                }
            }
            const std::string g = GlyphFor(b);
            if (fa && !g.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(g.c_str());
            }

            char tipBuf[128];
            std::snprintf(tipBuf, sizeof(tipBuf), "%s", b.Tooltip.c_str());
            if (ImGui::InputText("Tooltip", tipBuf, sizeof(tipBuf))) {
                b.Tooltip = tipBuf;
            }
        }

        if (b.Kind == ToolbarButtonKind::Command) {
            char cmdBuf[128];
            std::snprintf(cmdBuf, sizeof(cmdBuf), "%s", b.CommandId.c_str());
            if (ImGui::InputText("Command id", cmdBuf, sizeof(cmdBuf))) {
                b.CommandId = cmdBuf;
            }
            ImGui::TextDisabled("Pick a command:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##cmdsearch", "Filter commands...", cmdSearch_, sizeof(cmdSearch_));
            std::string q(cmdSearch_);
            ImGui::BeginChild("##cmdlist", ImVec2(0.0f, 90.0f), true);
            for (const std::string& n : cmdNames_) {
                if (!q.empty() && n.find(q) == std::string::npos) {
                    continue;
                }
                if (ImGui::Selectable(n.c_str(), b.CommandId == n)) {
                    b.CommandId = n;
                }
            }
            ImGui::EndChild();

            char argBuf[256];
            std::snprintf(argBuf, sizeof(argBuf), "%s", b.ArgsJson.c_str());
            if (ImGui::InputText("Args (JSON)", argBuf, sizeof(argBuf))) {
                b.ArgsJson = argBuf;
            }
        } else if (b.Kind == ToolbarButtonKind::Lua) {
            char luaBuf[2048];
            std::snprintf(luaBuf, sizeof(luaBuf), "%s", b.LuaCode.c_str());
            if (ImGui::InputTextMultiline("Lua code", luaBuf, sizeof(luaBuf), ImVec2(-1.0f, 120.0f))) {
                b.LuaCode = luaBuf;
            }
        } else {
            ImGui::TextDisabled("Separator (no settings).");
        }
    } else {
        ImGui::TextDisabled("Select or add a button.");
    }
    ImGui::EndChild();

    ImGui::Checkbox("Show toolbar", &editBuf_.Visible);
    ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
    if (ImGui::Button("Save", ImVec2(90.0f, 0.0f))) {
        cfg.Toolbar = editBuf_;
        ConfigManager::Save(cfg);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
