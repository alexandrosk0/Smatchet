#include "Ui/SmatchetActivityLogViewerUi.h"

#include "MarkdownPreviewRender.h"
#include "SmatchetLocalization.h"
#include "Tracker/CommentBlobFormatPure.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// The History field in the generic field-viewer shell: the whole plain `[Author] date` activity blob
/// as Markdown (the grid tooltip shows the same blob, capped at half the screen).
class ActivityLogViewerContent final : public IFieldViewerContent {
  public:
    explicit ActivityLogViewerContent(std::string blob) : m_Blob(std::move(blob)) {}

    std::string Title() const override { return SmatchetLocalization::T("history.title", "History"); }

    void DrawBody(AppController& /*app*/, bool /*readOnlyMode*/) override {
        if (m_Blob.empty()) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("history.none", "No history recorded."));
            return;
        }
        ImGui::BeginChild("##ActivityLogScroll", ImVec2(-FLT_MIN, 0.0f), true);
        const float width = ImGui::GetContentRegionAvail().x;
        EnsureEntryPlans(ImGui::GetFontSize(), width);
        MarkdownPreviewRender::Options opts;
        opts.mode = MarkdownPreviewRender::Mode::Full;
        // The blob carries other people's text: links are shown, not followed.
        opts.clickableLinks = false;
        const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
        for (size_t i = 0; i < m_EntryPlans.size(); ++i) {
            // Off-screen entries with a known height draw as a spacer instead of re-emitting every word
            // each frame (Pillar 1: the blob is up to ~16 KB) — same culling as the comments thread.
            float& height = m_EntryHeights[i];
            if (height > itemSpacingY && !ImGui::IsRectVisible(ImVec2(width, height))) {
                ImGui::Dummy(ImVec2(width, height - itemSpacingY)); // Dummy re-adds the trailing spacing
                continue;
            }
            const float startY = ImGui::GetCursorPosY();
            ImGui::PushID(static_cast<int>(i));
            MarkdownPreviewRender::RenderPlan(*m_EntryPlans[i], opts);
            if (i + 1 < m_EntryPlans.size()) {
                ImGui::Separator();
            }
            ImGui::PopID();
            height = ImGui::GetCursorPosY() - startY;
        }
        ImGui::EndChild();
    }

  private:
    /// One plan per activity entry, split on the converter's entry separator so off-screen entries can
    /// be culled. A plan caches word widths measured at the font size of its first render, so plans are
    /// rebuilt only when the font size changes; heights are re-measured when the width changes.
    void EnsureEntryPlans(float fontSize, float width) {
        if (m_EntryPlans.empty() || m_PlansFontSize != fontSize) {
            m_EntryPlans.clear();
            const std::string markdown = smatchet::tracker::PlainActivityBlobToMarkdown(m_Blob);
            const char* sep = smatchet::tracker::kActivityEntrySeparator;
            const size_t sepLen = std::strlen(sep);
            size_t start = 0;
            while (start <= markdown.size()) {
                size_t end = markdown.find(sep, start);
                if (end == std::string::npos) {
                    end = markdown.size();
                }
                MarkdownPreviewRender::PreviewPlanPtr plan = MarkdownPreviewRender::MakePlan();
                MarkdownPreviewRender::BuildPlan(markdown.substr(start, end - start), *plan);
                m_EntryPlans.push_back(std::move(plan));
                if (end == markdown.size()) {
                    break;
                }
                start = end + sepLen;
            }
            m_PlansFontSize = fontSize;
            m_HeightsWidth = -1.0f;
        }
        if (m_HeightsWidth != width || m_EntryHeights.size() != m_EntryPlans.size()) {
            m_EntryHeights.assign(m_EntryPlans.size(), 0.0f);
            m_HeightsWidth = width;
        }
    }

    std::string m_Blob; ///< copied at open: the ticket snapshot may be rebuilt while the window is open
    std::vector<MarkdownPreviewRender::PreviewPlanPtr> m_EntryPlans;
    /// Last drawn height of each entry (incl. separator + trailing spacing), <= 0 until measured.
    std::vector<float> m_EntryHeights;
    float m_PlansFontSize = 0.0f;
    float m_HeightsWidth = 0.0f;
};

} // namespace

std::unique_ptr<IFieldViewerContent> MakeActivityLogViewerContent(std::string blob) {
    return std::make_unique<ActivityLogViewerContent>(std::move(blob));
}
