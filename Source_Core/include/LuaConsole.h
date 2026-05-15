#ifndef LUA_CONSOLE_H
#define LUA_CONSOLE_H

#include "SmatchetLocalization.h"
#include "imgui.h"
#include <string>
#include <vector>

class LuaConsole {
  public:
    std::vector<std::string> Items;
    /// Persistent error log — not cleared by ClearLog(). Accumulates Lua automation errors
    /// across the session so they remain visible even after the normal output is cleared.
    std::vector<std::string> ErrorItems;
    bool AutoScroll = true;

    void ClearLog() { Items.clear(); }
    void ClearErrors() { ErrorItems.clear(); }

    void AddLog(const std::string& log) { Items.push_back(log); }

    /// Add a Lua error to the persistent error list (survives ClearLog). The scrolling log
    /// entry (`Items`) is handled separately by the caller — either via LuaLogInfoBind
    /// (automation runner) or an explicit AddLog call (console snippet). This avoids double-
    /// logging when both the info-sink and the error-sink path are active.
    void AddError(const std::string& msg) {
        ErrorItems.push_back(msg);
    }

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
                if (ImGui::Button(SmatchetLocalization::Label("lua.clear", "Clear", "LuaConsoleClear"))) {
                    ClearLog();
                }
                ImGui::SameLine();
                copy_to_clipboard = ImGui::Button(
                    SmatchetLocalization::Label("lua.copy_clipboard", "Copy to Clipboard", "LuaConsoleCopy"));
                ImGui::SameLine();
                ImGui::Checkbox(SmatchetLocalization::Label("lua.auto_scroll", "Auto-scroll", "LuaConsoleAutoScroll"),
                                &AutoScroll);
            } else {
                if (ImGui::Button(SmatchetLocalization::Label("lua.clear", "Clear", "LuaConsoleClear"))) {
                    ClearLog();
                }
                ImGui::SameLine();
                copy_to_clipboard = ImGui::Button(
                    SmatchetLocalization::Label("lua.copy_clipboard", "Copy to Clipboard", "LuaConsoleCopy"));
                ImGui::SameLine();
                ImGui::Checkbox(SmatchetLocalization::Label("lua.auto_scroll", "Auto-scroll", "LuaConsoleAutoScroll"),
                                &AutoScroll);
                ImGui::Separator();
            }
        }

        // Start clipboard capture before any rendered content so both the error panel
        // and the scrolling log are included in a single "Copy to Clipboard" operation.
        if (copy_to_clipboard) {
            ImGui::LogToClipboard();
        }

        // Persistent error panel — survives ClearLog; stays visible until ClearErrors().
        if (!ErrorItems.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.55f, 0.1f, 0.1f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.65f, 0.15f, 0.15f, 0.85f));
            const bool errOpen = ImGui::CollapsingHeader(
                SmatchetLocalization::Label("lua.errors_header", "Lua Errors", "LuaConsoleErrors"),
                ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleColor(2);
            if (errOpen) {
                if (!copy_to_clipboard) {
                    // Hide the "Clear Errors" button during clipboard copy so the button
                    // label itself doesn't land in the clipboard text.
                    if (ImGui::SmallButton(SmatchetLocalization::Label("lua.clear_errors", "Clear Errors", "LuaConsoleClearErrors"))) {
                        ClearErrors();
                    }
                }
                if (!ErrorItems.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    for (std::size_t i = 0; i < ErrorItems.size(); ++i) {
                        if (i > 0) {
                            ImGui::Separator();
                        }
                        ImGui::TextUnformatted(ErrorItems[i].c_str());
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::Separator();
        }

        float scroll_h = explicitScrollHeight;
        if (scroll_h <= 0.0f) {
            scroll_h = ImGui::GetContentRegionAvail().y;
        }

        const ImGuiChildFlags childFlags = ImGuiChildFlags_None;
        if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, scroll_h), childFlags, ImGuiWindowFlags_HorizontalScrollbar)) {
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

            if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        if (copy_to_clipboard) {
            ImGui::LogFinish();
        }

        if (showToolbar && !compactSingleLineToolbar && showBottomHint) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", SmatchetLocalization::T("lua.output", "Smatchet Automation Output"));
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
