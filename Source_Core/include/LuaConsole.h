#ifndef LUA_CONSOLE_H
#define LUA_CONSOLE_H

#include "imgui.h"
#include <string>
#include <vector>

class LuaConsole {
  public:
    std::vector<std::string> Items;
    bool AutoScroll = true;

    void ClearLog() { Items.clear(); }

    void AddLog(const std::string& log) { Items.push_back(log); }

    /** Toolbar + log scroll area (use inside an existing ImGui window). */
    void DrawPanelContents() {
        if (ImGui::Button("Clear"))
            ClearLog();
        ImGui::SameLine();
        bool copy_to_clipboard = ImGui::Button("Copy to Clipboard");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &AutoScroll);
        ImGui::Separator();

        const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            if (copy_to_clipboard)
                ImGui::LogToClipboard();

            for (const auto& item : Items) {
                ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                if (item.find("[ERROR]") != std::string::npos)
                    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                else if (item.find("[LUA]") != std::string::npos)
                    color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(item.c_str());
                ImGui::PopStyleColor();
            }

            if (copy_to_clipboard)
                ImGui::LogFinish();

            if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::TextDisabled("Smatchet Automation Output");
    }

    void Draw(const char* title, bool* p_open = nullptr) {
        ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, p_open)) {
            ImGui::End();
            return;
        }
        DrawPanelContents();
        ImGui::End();
    }
};

#endif






