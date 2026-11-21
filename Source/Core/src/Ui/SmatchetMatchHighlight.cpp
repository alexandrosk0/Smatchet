#include "Ui/SmatchetMatchHighlight.h"

#include "imgui.h"

#include <cfloat>
#include <cstring>

namespace SmatchetMatchHighlight {

void DrawText(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd,
              const smatchet::textmatch::ByteRange* ranges, std::size_t rangeCount) {
    if (dl == nullptr || text == nullptr) {
        return;
    }
    if (textEnd == nullptr) {
        textEnd = text + std::strlen(text);
    }
    const std::size_t textLen = static_cast<std::size_t>(textEnd - text);
    if (ranges != nullptr && rangeCount > 0) {
        // Exact glyph advances: ImGui::CalcTextSize rounds every width up, which would drift the mark to
        // the right of the letters it covers.
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const ImU32 markCol = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        for (std::size_t r = 0; r < rangeCount; ++r) {
            const std::size_t begin = ranges[r].Begin < textLen ? ranges[r].Begin : textLen;
            const std::size_t end = ranges[r].End < textLen ? ranges[r].End : textLen;
            if (begin >= end) {
                continue;
            }
            const char* b = text + begin;
            const char* e = text + end;
            const float x0 = pos.x + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, b).x;
            const float x1 = x0 + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, b, e).x;
            dl->AddRectFilled(ImVec2(x0 - 1.0f, pos.y), ImVec2(x1 + 1.0f, pos.y + fontSize), markCol, 2.0f);
        }
    }
    dl->AddText(pos, col, text, textEnd);
}

} // namespace SmatchetMatchHighlight
