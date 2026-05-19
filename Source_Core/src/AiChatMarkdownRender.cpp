// AiChatMarkdownRender — ImGui-side renderer. Pure markdown parsing lives in
// `AiChatMarkdownParse.cpp` so the test rig can exercise `ParseBlocks` without
// linking ImGui. This TU only consumes `ParseBlocks` + emits widgets.

#include "AiChatMarkdownRender.h"

#include "SmatchetImGuiFonts.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace AiChatMarkdownRender {

void Render(const std::string& md, const char* scopeId, float panelWidth) {
    const std::vector<Block> blocks = ParseBlocks(md);
    if (blocks.empty()) {
        return;
    }
    ImGui::PushID(scopeId);
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float framePad2 = style.FramePadding.y * 2.0f;
    const float wrapWidth = (panelWidth > 32.0f) ? (panelWidth - 16.0f) : panelWidth;

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const Block& b = blocks[i];
        const bool mono = b.UseMonospace;
        ImFont* monoFont = SmatchetGetPreviewFonts().Mono;
        if (mono && monoFont) {
            ImGui::PushFont(monoFont);
        }
        float h = 0.0f;
        if (mono) {
            const std::size_t lines = static_cast<std::size_t>(std::count(b.Text.begin(), b.Text.end(), '\n')) + 1u;
            h = static_cast<float>(lines) * lineHeight;
        } else {
            const ImVec2 sz = ImGui::CalcTextSize(b.Text.c_str(), nullptr, false, wrapWidth);
            h = sz.y + lineHeight;
        }
        h += framePad2 + 4.0f;
        ImGui::InputTextMultiline("##blk", const_cast<char*>(b.Text.c_str()),
                                  static_cast<std::size_t>(b.Text.size()) + 1u, ImVec2(-1.0f, h),
                                  ImGuiInputTextFlags_ReadOnly);
        if (mono && monoFont) {
            ImGui::PopFont();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

} // namespace AiChatMarkdownRender
