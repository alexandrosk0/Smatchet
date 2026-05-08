#include "SmatchetFieldRender.h"

#include "imgui.h"

void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip) {
    ImGui::AlignTextToFramePadding();
    const std::string& displayValue = rawValue;

    bool hasNewline = false;
    std::string singleLine = displayValue;
    const size_t pos = singleLine.find_first_of("\r\n");
    if (pos != std::string::npos) {
        singleLine.erase(pos);
        hasNewline = true;
    }

    const ImVec2 textSize = ImGui::CalcTextSize(singleLine.c_str());
    const bool horizontallyClipped = (availWidth > 0.0f && textSize.x > availWidth + 1.0f);

    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ImGui::TextUnformatted(singleLine.c_str());
    if (disabled) {
        ImGui::PopStyleColor();
    }

    const std::string& tipSource = (rawForTooltip && !rawForTooltip->empty()) ? *rawForTooltip : displayValue;
    if (tooltipsEnabled && (hasNewline || horizontallyClipped) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tipSource.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}






