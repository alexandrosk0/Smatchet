// AiChatMarkdownRender — ImGui-side renderer. Pure markdown parsing lives in
// `AiChatMarkdownParse.cpp` so the test rig can exercise `ParseBlocks` without
// linking ImGui. This TU only consumes `ParseBlocks` + emits widgets.
//
// Rendering policy: ALL blocks render with the monospace font (Consolas) to
// match the Smatchet Lua editor's look. Heading / paragraph / list-item /
// quote blocks soft-wrap inside their InputTextMultiline; code fences /
// tables keep their original line breaks verbatim. Block height is computed
// from the wrapped line count using the active (mono) font metrics.

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

namespace {

// Height for a soft-wrapping block (heading / paragraph / list-item / quote).
// Walks the text manually and counts how many wrapped lines the ImGui InputText
// will need at `wrapWidth`, using the currently-pushed font. The render path
// pushes Mono before calling this so `CalcTextSize` measures with monospace
// glyphs — matches what the widget will actually draw.
float SoftWrapHeight(const std::string& text, float wrapWidth, float lineHeight) {
    std::size_t lineCount = 1u;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        std::size_t lineEnd = text.find('\n', i);
        if (lineEnd == std::string::npos) {
            lineEnd = n;
        }
        // Measure the source line; if it overflows `wrapWidth`, count extra
        // wrapped lines proportional to overflow. Cheap heuristic — exact
        // measurement is unnecessary because we add a one-line slack below.
        const float w = ImGui::CalcTextSize(text.c_str() + i, text.c_str() + lineEnd).x;
        if (w > wrapWidth && wrapWidth > 0.0f) {
            const std::size_t extra = static_cast<std::size_t>(w / wrapWidth);
            lineCount += extra;
        }
        if (lineEnd < n) {
            ++lineCount;
            i = lineEnd + 1;
        } else {
            break;
        }
    }
    return static_cast<float>(lineCount) * lineHeight;
}

} // namespace

void Render(const std::string& md, const char* scopeId, float panelWidth) {
    const std::vector<Block> blocks = ParseBlocks(md);
    if (blocks.empty()) {
        return;
    }
    ImGui::PushID(scopeId);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float framePad2 = style.FramePadding.y * 2.0f;
    const float wrapWidth = (panelWidth > 32.0f) ? (panelWidth - 16.0f) : panelWidth;

    ImFont* monoFont = SmatchetGetPreviewFonts().Mono;
    const bool havePushedFont = (monoFont != nullptr);
    if (havePushedFont) {
        ImGui::PushFont(monoFont);
    }
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const Block& b = blocks[i];
        // Code fences + tables keep their original line breaks verbatim, so
        // their height is line-count × line-height; everything else may wrap,
        // so we estimate via SoftWrapHeight.
        float h = 0.0f;
        if (b.UseMonospace) {
            const std::size_t lines = static_cast<std::size_t>(std::count(b.Text.begin(), b.Text.end(), '\n')) + 1u;
            h = static_cast<float>(lines) * lineHeight;
        } else {
            h = SoftWrapHeight(b.Text, wrapWidth, lineHeight);
        }
        h += framePad2 + 4.0f;
        ImGui::InputTextMultiline("##blk", const_cast<char*>(b.Text.c_str()),
                                  static_cast<std::size_t>(b.Text.size()) + 1u, ImVec2(-1.0f, h),
                                  ImGuiInputTextFlags_ReadOnly);
        ImGui::PopID();
    }

    if (havePushedFont) {
        ImGui::PopFont();
    }
    ImGui::PopID();
}

} // namespace AiChatMarkdownRender
