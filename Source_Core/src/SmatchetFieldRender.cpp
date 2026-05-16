#include "SmatchetFieldRender.h"

#include "CppSyntaxHighlight.h"
#include "MarkdownPreviewRender.h"

#include "imgui.h"

namespace {

// Set once at app/blame-config load time. Empty = no field opts into syntax highlight.
std::string g_callstackFieldId;

bool IsCallstackField(const std::string* fieldId) {
    return fieldId != nullptr && !fieldId->empty() && !g_callstackFieldId.empty() && *fieldId == g_callstackFieldId;
}

} // namespace

void SetCallstackFieldIdHint(const std::string& fieldId) { g_callstackFieldId = fieldId; }

void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip, bool renderMarkdown, const std::string* fieldId) {
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
    const bool isCallstack = IsCallstackField(fieldId);

    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    if (isCallstack) {
        DrawColoredCppLine(singleLine.c_str());
    } else {
        ImGui::TextUnformatted(singleLine.c_str());
    }

    const std::string& tipSource = (rawForTooltip && !rawForTooltip->empty()) ? *rawForTooltip : displayValue;
    if (tooltipsEnabled && (hasNewline || horizontallyClipped) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (isCallstack) {
            DrawColoredCppText(tipSource.c_str());
        } else {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            if (renderMarkdown) {
                MarkdownPreviewRender::Options opts;
                opts.mode = MarkdownPreviewRender::Mode::Tooltip;
                opts.clickableLinks = false;
                MarkdownPreviewRender::Render(tipSource, opts);
            } else {
                ImGui::TextUnformatted(tipSource.c_str());
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }

    if (disabled) {
        ImGui::PopStyleColor();
    }
}
