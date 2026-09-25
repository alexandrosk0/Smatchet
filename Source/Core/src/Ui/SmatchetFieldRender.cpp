#include "SmatchetFieldRender.h"

#include "CppSyntaxHighlight.h"
#include "FieldPreviewLinePure.h"
#include "MarkdownPreviewRender.h"
#include "TicketFieldEditorDescriptionPure.h"
#include "Tracker/CommentBlobFormatPure.h"

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

enum class TooltipSource { Markdown, PlainActivityBlob };

struct TooltipPlanRef {
    const MarkdownPreviewRender::PreviewPlan& Plan;
    bool ContentChanged;
};

// Only one tooltip is visible at a time, so a single-slot plan cache skips the conversion,
// md4c parse and per-word measurement on every frame the same value stays hovered (Pillar 1:
// a comment thread or history blob is up to ~16 KB). Keyed on source kind, content AND font
// size — the plan caches word widths measured at the font size of its first render.
TooltipPlanRef CachedTooltipPlan(const std::string& source, TooltipSource kind) {
    static MarkdownPreviewRender::PreviewPlanPtr s_plan;
    static std::uint64_t s_hash = 0;
    static std::size_t s_size = 0;
    static float s_fontSize = 0.0f;
    static TooltipSource s_kind = TooltipSource::Markdown;
    const std::uint64_t hash = MarkdownPreviewRender::HashContent(source);
    const float fontSize = ImGui::GetFontSize();
    const bool sameContent = s_plan && hash == s_hash && source.size() == s_size && kind == s_kind;
    if (!sameContent || fontSize != s_fontSize) {
        MarkdownPreviewRender::PreviewPlanPtr plan = MarkdownPreviewRender::MakePlan();
        MarkdownPreviewRender::BuildPlan(
            kind == TooltipSource::PlainActivityBlob ? smatchet::tracker::PlainActivityBlobToMarkdown(source) : source,
            *plan);
        s_plan = std::move(plan);
        s_hash = hash;
        s_size = source.size();
        s_fontSize = fontSize;
        s_kind = kind;
    }
    return TooltipPlanRef{*s_plan, !sameContent};
}

// Content wraps at a fixed width and the child auto-fits both axes, capped at half the display
// height; past that it scrolls (the wheel reaches it via RouteWheelToScrollableTooltipBeforeNewFrame).
// The child id is shared by every tooltip, so its scroll is reset whenever the content changes —
// otherwise a thread scrolled to the bottom would open the next hovered value mid-way.
void RenderTooltipBody(const std::string& source, TooltipSource kind) {
    const TooltipPlanRef ref = CachedTooltipPlan(source, kind);
    const float wrapWidth = MarkdownTooltipWrapWidth();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(wrapWidth + style.ScrollbarSize + style.WindowPadding.x * 2.0f, ImGui::GetIO().DisplaySize.y * 0.5f));
    if (ref.ContentChanged) {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    }
    ImGui::BeginChild("##markdown_tooltip_scroll", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(wrapWidth);
    MarkdownPreviewRender::Options opts;
    opts.mode = MarkdownPreviewRender::Mode::Tooltip;
    opts.clickableLinks = false;
    opts.wrapWidth = wrapWidth;
    MarkdownPreviewRender::RenderPlan(ref.Plan, opts);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void RenderTooltip(const std::string& source, TooltipSource kind) {
    if (source.empty()) {
        return;
    }
    ImGui::BeginTooltip();
    RenderTooltipBody(source, kind);
    ImGui::EndTooltip();
}

} // namespace

void RenderMarkdownTooltip(const std::string& markdown) { RenderTooltip(markdown, TooltipSource::Markdown); }

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
    // renderMarkdown: raw-markdown bodies (the grid pending-save cell, the mobile detail list).
    // ADF/HTML description fields convert their rich value first in TicketFieldEditor.cpp, then
    // reach the same RenderMarkdownTooltip path. History is recognised here from the field id so
    // every caller gets its Markdown tooltip, not only the ones that remember to ask.
    const bool isActivityBlob = fieldId != nullptr && IsMarkdownActivityFieldId(*fieldId);
    // For callstack fields the cell always shows only the first line (singleLine);
    // show the full-text tooltip on hover regardless of clipping so the user can
    // read the complete stack even when the first line fits in the column width.
    if (tooltipsEnabled && (hasNewline || horizontallyClipped || isCallstack) && ImGui::IsItemHovered()) {
        if (isCallstack) {
            ImGui::BeginTooltip();
            // Slice 7 — semantic callstack tokenizer.
            DrawColoredCallstackText(tipSource.c_str());
            ImGui::EndTooltip();
        } else if (isActivityBlob) {
            RenderTooltip(tipSource, TooltipSource::PlainActivityBlob);
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
