#include "LuaConsolePlugin.h"
#include "AppController.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

namespace {

void ReloadSmatchetHooksSetupScript(AppController& app) {
    app.RunLuaSetupScript("SmatchetHooks.lua");
}

} // namespace

void LuaConsolePlugin::OnEarlyInit(AppController& app) {
    app.AddAutomationLogSink([this](const std::string& msg) { console_.AddLog(std::string("[LUA] ") + msg); });
    const std::string autoPath = app.ResolveLuaScriptPath("Automation.lua");
    const std::string hooksPath = app.ResolveLuaScriptPath("SmatchetHooks.lua");
    const auto fileReadable = [](const std::string& p) -> bool {
        if (p.empty()) {
            return false;
        }
        std::ifstream f(p, std::ios::binary);
        return f.is_open();
    };
    LOG_INFO("LuaConsolePlugin: Automation.lua resolved=\"%s\" readable=%s", autoPath.c_str(),
             fileReadable(autoPath) ? "yes" : "no");
    LOG_INFO("LuaConsolePlugin: SmatchetHooks.lua resolved=\"%s\" readable=%s", hooksPath.c_str(),
             fileReadable(hooksPath) ? "yes" : "no");
}

void LuaConsolePlugin::LoadAutomationEditorFile(AppController& app) {
    const std::string path = app.ResolveLuaScriptPath("Automation.lua");
    if (path.empty()) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    if (file) {
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string content = ss.str();
        automationEditorBuffer_.resize(std::max<size_t>(content.size() + 1024, 16384));
        std::copy(content.begin(), content.end(), automationEditorBuffer_.begin());
        automationEditorBuffer_[content.size()] = '\0';
    } else {
        automationEditorBuffer_.resize(16384, '\0');
    }
    automationEditorLoaded_ = true;
}

void LuaConsolePlugin::SaveAutomationEditorFile(AppController& app) {
    const std::string path = app.ResolveLuaScriptPath("Automation.lua");
    if (path.empty()) {
        return;
    }
    std::ofstream file(path, std::ios::binary);
    if (file && !automationEditorBuffer_.empty()) {
        file << automationEditorBuffer_.data();
    }
}

void LuaConsolePlugin::LoadHooksEditorFile(AppController& app) {
    const std::string path = app.ResolveLuaScriptPath("SmatchetHooks.lua");
    if (path.empty()) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    if (file) {
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string content = ss.str();
        hooksEditorBuffer_.resize(std::max<size_t>(content.size() + 1024, 16384));
        std::copy(content.begin(), content.end(), hooksEditorBuffer_.begin());
        hooksEditorBuffer_[content.size()] = '\0';
    } else {
        hooksEditorBuffer_.resize(16384, '\0');
        hooksEditorBuffer_[0] = '\0';
    }
    hooksEditorLoaded_ = true;
}

void LuaConsolePlugin::SaveHooksEditorFile(AppController& app) {
    const std::string path = app.ResolveLuaScriptPath("SmatchetHooks.lua");
    if (path.empty()) {
        return;
    }
    std::ofstream file(path, std::ios::binary);
    if (file && !hooksEditorBuffer_.empty()) {
        file << hooksEditorBuffer_.data();
    }
}

void LuaConsolePlugin::OnDraw(AppController& app) {
    if (!g_ui.showLuaAutomationWindow) {
        app.DrawLuaWindows();
        return;
    }

    const bool wantFocus = g_ui.requestLuaAutomationFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
    
    ImGui::SetNextWindowSize(ImVec2(650, 720), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    
    if (!ImGui::Begin("Lua Automation & Scripting", &g_ui.showLuaAutomationWindow)) {
        ImGui::End();
        ImGui::PopStyleVar();
        if (wantFocus) g_ui.requestLuaAutomationFocus = false;
        return;
    }
    
    if (wantFocus) {
        ImGui::SetWindowFocus();
        g_ui.requestLuaAutomationFocus = false;
    }

    if (!automationEditorLoaded_) LoadAutomationEditorFile(app);
    if (!hooksEditorLoaded_) LoadHooksEditorFile(app);

    // Sidebar/Top info area could go here, but let's stick to a clean tabbed interface
    if (ImGui::BeginTabBar("##lua_main_tabs", ImGuiTabBarFlags_None)) {
        
        // --- Tab 1: Global Actions ---
        ImGuiTabItemFlags scriptingFlags = ImGuiTabItemFlags_None;
        if (g_ui.requestScriptsWindowFocus) {
            scriptingFlags |= ImGuiTabItemFlags_SetSelected;
            g_ui.requestScriptsWindowFocus = false;
        }

        if (ImGui::BeginTabItem("Tools & Actions", nullptr, scriptingFlags)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quick Access Tools");
            ImGui::SameLine();
            ImGui::TextDisabled("(Registered via ui.register_global_action)");
            ImGui::Separator();
            ImGui::Spacing();

            const auto globalNames = app.GetLuaGlobalActionNames();
            if (globalNames.empty()) {
                ImGui::TextDisabled("No global actions registered.");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                ImGui::TextWrapped("Global actions are defined in SmatchetHooks.lua using 'ui.register_global_action(name, callback)'.");
                ImGui::PopStyleColor();
            } else {
                // Layout buttons in a wrapping grid or a clean list
                for (const auto& name : globalNames) {
                    ImGui::PushID(name.c_str());
                    if (ImGui::Button(name.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 32.0f))) {
                        std::string err;
                        if (!app.ExecuteLuaGlobalAction(name, err)) {
                            g_ui.gridEditError = "Lua Error: " + err;
                        } else {
                            g_ui.gridEditSuccess = "Executed: " + name;
                        }
                    }
                    if (ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.48f < ImGui::GetContentRegionMax().x) {
                        ImGui::SameLine();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTabItem();
        }

        // --- Tab 2: Automation Script ---
        if (ImGui::BeginTabItem("Batch Automation")) {
            ImGui::Spacing();
            
            // Header & Toolbar
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "Automation Script");
            ImGui::SameLine();
            ImGui::TextDisabled("(Automation.lua)");
            
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 220);
            if (ImGui::Button("Save && Run on Selection", ImVec2(210, 0))) {
                SaveAutomationEditorFile(app);
                std::vector<std::string> selectedIds;
                const auto snap = app.GetActiveTicketsSnapshot();
                const auto& tickets = *snap;

                // Selection logic (keeping existing functionality but cleaner)
                for (int r : g_ui.gridState.RectSel.Rows) {
                    if (r >= 0 && static_cast<size_t>(r) < g_ui.cachedSortedIndices.size()) {
                        const size_t originalIdx = g_ui.cachedSortedIndices[static_cast<size_t>(r)];
                        if (originalIdx < tickets.size()) selectedIds.push_back(tickets[originalIdx].id);
                    }
                }
                if (g_ui.gridState.RectSel.Active) {
                    const int minR = g_ui.gridState.RectSel.MinRow();
                    const int maxR = g_ui.gridState.RectSel.MaxRow();
                    for (int r = minR; r <= maxR; ++r) {
                        if (r >= 0 && static_cast<size_t>(r) < g_ui.cachedSortedIndices.size()) {
                            const size_t originalIdx = g_ui.cachedSortedIndices[static_cast<size_t>(r)];
                            if (originalIdx < tickets.size()) selectedIds.push_back(tickets[originalIdx].id);
                        }
                    }
                }
                std::sort(selectedIds.begin(), selectedIds.end());
                selectedIds.erase(std::unique(selectedIds.begin(), selectedIds.end()), selectedIds.end());
                app.RunAutoScript("Automation.lua", selectedIds);
            }
            ImGui::EndGroup();
            
            ImGui::Separator();
            ImGui::Spacing();

            // Editor
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // Ensure standard font for editor
            ImGui::InputTextMultiline("##lua_automation_editor", automationEditorBuffer_.data(),
                                      automationEditorBuffer_.size(), ImVec2(-1.0f, ImGui::GetContentRegionAvail().y * 0.55f),
                                      ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopFont();
            
            ImGui::Spacing();
            ImGui::TextDisabled("Note: This script should define a 'process_ticket(ticket)' function.");
            
            ImGui::EndTabItem();
        }

        // --- Tab 3: System Hooks ---
        if (ImGui::BeginTabItem("System Hooks")) {
            ImGui::Spacing();
            
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Integration Hooks");
            ImGui::SameLine();
            ImGui::TextDisabled("(SmatchetHooks.lua)");
            
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 260);
            if (ImGui::Button("Save && Reload", ImVec2(120, 0))) {
                SaveHooksEditorFile(app);
                ReloadSmatchetHooksSetupScript(app);
            }
            ImGui::SameLine();
            if (ImGui::Button("Disk Reload", ImVec2(120, 0))) {
                hooksEditorLoaded_ = false;
                LoadHooksEditorFile(app);
            }
            ImGui::EndGroup();
            
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
            ImGui::InputTextMultiline("##lua_hooks_editor", hooksEditorBuffer_.data(), hooksEditorBuffer_.size(),
                                      ImVec2(-1.0f, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopFont();
            
            ImGui::Spacing();
            ImGui::TextDisabled("Controls: Field Displays, Ticket Actions, MCP Tools, and Custom Windows.");
            
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // --- Footer: Console & Logs ---
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Output Log");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 70);
    if (ImGui::SmallButton("Clear")) {
        console_.ClearLog();
    }
    ImGui::EndGroup();
    
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
    ImGui::BeginChild("##console_area", ImVec2(-1, -1), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    console_.DrawPanelContents();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();

    app.DrawLuaWindows();
}






