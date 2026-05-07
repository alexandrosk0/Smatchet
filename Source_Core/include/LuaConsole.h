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

    /**
     * @param showToolbar Clear / Copy / Auto-scroll row.
     * @param explicitScrollHeight If > 0, scrolling region uses this height (px); if <= 0, uses remaining
     *                             content height below the toolbar.
     * @param compactSingleLineToolbar One toolbar row, no extra separator/footer hint.
     * @param showBottomHint Legacy footer line under the log (ignored when compactSingleLineToolbar).
     */
    void DrawPanelContents(bool showToolbar = true, float explicitScrollHeight = -1.0f, bool compactSingleLineToolbar = false,
                           bool showBottomHint = true) {
        bool copy_to_clipboard = false;
        if (showToolbar) {
            if (compactSingleLineToolbar) {
                if (ImGui::Button("Clear")) {
                    ClearLog();
                }
                ImGui::SameLine();
                copy_to_clipboard = ImGui::Button("Copy to Clipboard");
                ImGui::SameLine();
                ImGui::Checkbox("Auto-scroll", &AutoScroll);
            } else {
                if (ImGui::Button("Clear")) {
                    ClearLog();
                }
                ImGui::SameLine();
                copy_to_clipboard = ImGui::Button("Copy to Clipboard");
                ImGui::SameLine();
                ImGui::Checkbox("Auto-scroll", &AutoScroll);
                ImGui::Separator();
            }
        }

        float scroll_h = explicitScrollHeight;
        if (scroll_h <= 0.0f) {
            scroll_h = ImGui::GetContentRegionAvail().y;
        }

        const ImGuiChildFlags childFlags = ImGuiChildFlags_None;
        if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, scroll_h), childFlags, ImGuiWindowFlags_HorizontalScrollbar)) {
            if (copy_to_clipboard) {
                ImGui::LogToClipboard();
            }

            for (const auto& item : Items) {
                ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                if (item.find("[ERROR]") != std::string::npos) {
                    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                } else if (item.find("[LUA]") != std::string::npos) {
                    color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(item.c_str());
                ImGui::PopStyleColor();
            }

            if (copy_to_clipboard) {
                ImGui::LogFinish();
            }

            if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        if (showToolbar && !compactSingleLineToolbar && showBottomHint) {
            ImGui::Separator();
            ImGui::TextDisabled("Smatchet Automation Output");
        }
    }

    void Draw(const char* title, bool* p_open = nullptr) {
        ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, p_open)) {
            ImGui::End();
            return;
        }
        DrawPanelContents(true, -1.0f, false, true);
        ImGui::End();
    }
};

#endif
