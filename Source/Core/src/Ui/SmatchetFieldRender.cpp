#include "SmatchetFieldRender.h"

#include "CppSyntaxHighlight.h"
#include "FieldPreviewLinePure.h"
#include "MarkdownPreviewRender.h"

#include "imgui.h"

namespace {

// Set once at app/annotate-config load time. Empty = no field opts into syntax highlight.
std::string g_callstackFieldId;

bool IsCallstackField(const std::string* fieldId) {
    return fieldId != nullptr && !fieldId->empty() && !g_callstackFieldId.empty() && *fieldId == g_callstackFieldId;
}

} // namespace

void SetCallstackFieldIdHint(const std::string& fieldId) { g_callstackFieldId = fieldId; }

bool IsCallstackFieldId(const std::string& fieldId) {
    return !fieldId.empty() && !g_callstackFieldId.empty() && fieldId == g_callstackFieldId;
}

void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip, bool renderMarkdown, const std::string* fieldId) {
    ImGui::AlignTextToFramePadding();
    const std::string& displayValue = rawValue;

    // First line with VISIBLE content, not literally the first line: a value opening
    // with a blank line (GitHub issue bodies routinely do) otherwise drew an empty
    // cell for a ticket that has full text. Tooltip below still shows the raw value.
    const smatchet::field_preview::PreviewLine preview = smatchet::field_preview::FirstVisibleLine(displayValue);
    const std::string& singleLine = preview.Text;
    const bool hasNewline = preview.HasMoreLines;

    const ImVec2 textSize = ImGui::CalcTextSize(singleLine.c_str());
    const bool horizontallyClipped = (availWidth > 0.0f && textSize.x > availWidth + 1.0f);
    const bool isCallstack = IsCallstackField(fieldId);

    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    // Group wrap so IsItemHovered() below treats the whole cell as one item.
    // DrawColoredCppLine emits one TextUnformatted per token; without the group,
    // hover would only register on the last token and suppress the tooltip.
    ImGui::BeginGroup();
    if (isCallstack) {
        // Slice 7 — semantic callstack tokenizer (per-element colours for
        // Module!Class::Method() [Path\File.ext:Line]). Falls back to cpp
        // syntax on non-canonical rows.
        DrawColoredCallstackLine(singleLine.c_str());
    } else {
        ImGui::TextUnformatted(singleLine.c_str());
    }
    ImGui::EndGroup();

    const std::string& tipSource = (rawForTooltip && !rawForTooltip->empty()) ? *rawForTooltip : displayValue;
    // Safety net for non-ADF text fields only. ADF/description fields are handled
    // by the lazy tooltip in TicketFieldEditor.cpp (renderPlainText + RenderTextEditor)
    // and never reach this path with renderMarkdown=true.
    // For callstack fields the cell always shows only the first line (singleLine);
    // show the full-text tooltip on hover regardless of clipping so the user can
    // read the complete stack even when the first line fits in the column width.
    if (tooltipsEnabled && (hasNewline || horizontallyClipped || isCallstack) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (isCallstack) {
            // Slice 7 — semantic callstack tokenizer.
            DrawColoredCallstackText(tipSource.c_str());
        } else {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            if (renderMarkdown) {
                MarkdownPreviewRender::Options opts;
                opts.mode = MarkdownPreviewRender::Mode::Tooltip;
                opts.clickableLinks = false;
                opts.wrapWidth = ImGui::GetFontSize() * 48.0f;
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
