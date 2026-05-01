#include "LuaConsolePlugin.h"
#include "AppController.h"

void LuaConsolePlugin::OnEarlyInit(AppController& app) {
    app.AddAutomationLogSink([this](const std::string& msg) { console_.AddLog(std::string("[LUA] ") + msg); });
}

void LuaConsolePlugin::OnDraw(AppController& app) {
    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lua & Automation")) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Run Lua Automation")) {
        app.RunAutoScript("Automation.lua");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload FieldDisplay.lua")) {
        app.RunLuaSetupScript("FieldDisplay.lua");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Automation.lua per active ticket; FieldDisplay.lua once)");
    ImGui::Separator();

    console_.DrawPanelContents();

    ImGui::End();
}
