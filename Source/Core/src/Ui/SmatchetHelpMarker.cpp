#include "SmatchetHelpMarker.h"

#include "IconsFontAwesome6.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "imgui.h"

namespace SmatchetHelpMarker {

void RenderText(const char* fullText) {
    if (!fullText || fullText[0] == '\0') {
        return;
    }
    // Re-check per call — the FA merge state is invalidated by font hot-reload
    // (language / font-size change rebuilds the atlas mid-session).
    const char* glyph = SmatchetAreFaIconsLoaded() ? ICON_FA_CIRCLE_QUESTION : "(?)";
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(fullText);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void Render(const char* tooltipKey, const char* tooltipFallback) {
    RenderText(SmatchetLocalization::T(tooltipKey, tooltipFallback));
}

} // namespace SmatchetHelpMarker
