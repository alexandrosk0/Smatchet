#include "SmatchetFieldRender.h"

#include "CppSyntaxHighlight.h"
#include "FieldPreviewLinePure.h"
#include "MarkdownPreviewRender.h"

#include "imgui.h"

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

// Set once at app/annotate-config load time. Empty = no field opts into syntax highlight.
std::string g_callstackFieldId;

bool IsCallstackField(const std::string* fieldId) {
    return fieldId != nullptr && !fieldId->empty() && !g_callstackFieldId.empty() && *fieldId == g_callstackFieldId;
}

float MarkdownTooltipWrapWidth() { return ImGui::GetFontSize() * 48.0f; }

// Only one tooltip is visible at a time, so a single-slot plan cache skips the md4c parse and
// per-word measurement on every frame the same value stays hovered (Pillar 1: a comment thread
// or history blob is up to ~16 KB). Keyed on content AND font size — the plan caches word
// widths measured at the font size of its first render.
const MarkdownPreviewRender::PreviewPlan& CachedTooltipPlan(const std::string& markdown) {
    static MarkdownPreviewRender::PreviewPlanPtr s_plan;
    static std::uint64_t s_hash = 0;
    static std::size_t s_size = 0;
    static float s_fontSize = 0.0f;
    const std::uint64_t hash = MarkdownPreviewRender::HashContent(markdown);
    const float fontSize = ImGui::GetFontSize();
    if (!s_plan || hash != s_hash || markdown.size() != s_size || fontSize != s_fontSize) {
        MarkdownPreviewRender::PreviewPlanPtr plan = MarkdownPreviewRender::MakePlan();
        MarkdownPreviewRender::BuildPlan(markdown, *plan);
        s_plan = std::move(plan);
        s_hash = hash;
        s_size = markdown.size();
        s_fontSize = fontSize;
    }
    return *s_plan;
}

} // namespace

void RenderMarkdownTooltipBody(const std::string& markdown) {
    if (markdown.empty()) {
        return;
    }
    const float wrapWidth = MarkdownTooltipWrapWidth();
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetIO().DisplaySize.y * 0.5f));
    ImGui::BeginChild("##markdown_tooltip_scroll", ImVec2(wrapWidth + ImGui::GetStyle().ScrollbarSize, 0.0f),
                      ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(wrapWidth);
    MarkdownPreviewRender::Options opts;
    opts.mode = MarkdownPreviewRender::Mode::Tooltip;
    opts.clickableLinks = false;
    opts.wrapWidth = wrapWidth;
    MarkdownPreviewRender::RenderPlan(CachedTooltipPlan(markdown), opts);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void RenderMarkdownTooltip(const std::string& markdown) {
    if (markdown.empty()) {
        return;
    }
    ImGui::BeginTooltip();
    RenderMarkdownTooltipBody(markdown);
    ImGui::EndTooltip();
}

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
    // renderMarkdown: History blobs and raw-markdown bodies (the grid pending-save cell, the
    // mobile detail list). ADF/HTML description fields convert their rich value first in
    // TicketFieldEditor.cpp, then reach the same RenderMarkdownTooltip path.
    // For callstack fields the cell always shows only the first line (singleLine);
    // show the full-text tooltip on hover regardless of clipping so the user can
    // read the complete stack even when the first line fits in the column width.
    if (tooltipsEnabled && (hasNewline || horizontallyClipped || isCallstack) && ImGui::IsItemHovered()) {
        if (isCallstack) {
            ImGui::BeginTooltip();
            // Slice 7 — semantic callstack tokenizer.
            DrawColoredCallstackText(tipSource.c_str());
            ImGui::EndTooltip();
        } else if (renderMarkdown) {
            RenderMarkdownTooltip(tipSource);
        } else {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(MarkdownTooltipWrapWidth());
            ImGui::TextUnformatted(tipSource.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    if (disabled) {
        ImGui::PopStyleColor();
    }
}
