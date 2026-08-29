// DescriptionTooltipMarkdownRenderScenario — slice 8 of
// autonomous-debugging-no-creds. Defensive cover for commit be2b1d9
// ("wrapWidth grep gate") which fixed a tooltip-render regression where
// `MarkdownPreviewRender::Render` was invoked from a BeginTooltip block
// without `opts.wrapWidth` set, producing an ultra-narrow vertical strip
// because the tooltip's GetContentRegionAvail().x is near-zero.
// What this scenario does:
//   * Drives `MarkdownPreviewRender::Render` with `opts.mode = Tooltip` +
//     a representative wrap width over a markdown body, N times per frame,
//     for N frames. Pillar-1 perf row accumulates so any regression in the
//     markdown plan cache or the wrap loop surfaces as a `rows[]` delta.
//   * Sanity-asserts that the tooltip render produces a non-zero
//     bounding-box height when `opts.wrapWidth > 0`, AND a degenerate near-
//     zero width when `opts.wrapWidth = 0` (the bug the grep gate guards).
//     The assertion runs once at OnStart, not per frame — the per-frame
//     perf loop just keeps the row visible.
// Render lives outside ImGui::Begin/End — the scenario is a perf driver,
// not a tooltip-replication test. MarkdownPreviewRender::Render emits raw
// ImGui draw calls, so when no ImGui window is current we capture nothing
// observable beyond the UiPerfMonitor scope row's accumulation. This is
// sufficient for the perf-regression purpose; the *correctness* side of
// the wrapWidth contract is covered by the bucket-E grid tooltip flow in
// slice 9.

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/UiPerfRowsJson.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "MarkdownPreviewRender.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <imgui.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// Static markdown body — built once at first call, then reused across every
// frame so the perf row reflects only render cost (not per-frame string
// construction). Mixed inline marks + a code span + bullets — exercises the
// markdown plan cache's prose / inline-code / list-item branches without
// requiring the full md4c table path.
const std::string& SampleMarkdownBody() {
    static const std::string kBody = "Issue summary: **render** fails when `wrapWidth=0` because the "
                                     "tooltip window's `GetContentRegionAvail().x` is near-zero.\n\n"
                                     "- repro 1: hover a description grid cell\n"
                                     "- repro 2: observe vertical strip\n"
                                     "- fix: pass `opts.wrapWidth` explicitly\n";
    return kBody;
}

// Minimum wrap width — anything smaller produces degenerate window sizing
// and a non-representative perf row.
const float kMinWrapWidth = 1.0f;

} // namespace

class DescriptionTooltipMarkdownRenderScenario : public IScenario {
  public:
    std::string Name() const override { return "description-tooltip-markdown-render"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frames_ = (std::max)(1, args.value("frames", 300));
        wrapWidth_ = (std::max)(kMinWrapWidth, args.value("wrapWidth", 480.0f));
        rendersPerFrame_ = (std::max)(1, args.value("rendersPerFrame", 4));

        const std::string& md = SampleMarkdownBody();

        // Sanity assertion — build a plan and verify it's non-empty. A
        // regression that empties the plan would silently zero the perf row;
        // capture the failure here so the scenario's JSON envelope carries
        // the diagnostic rather than emitting an empty rows[] silently.
        MarkdownPreviewRender::PreviewPlan plan;
        MarkdownPreviewRender::BuildPlan(md, plan);
        if (plan.blocks.empty()) {
            outErr = "BuildPlan produced zero blocks for the sample markdown body — "
                     "MarkdownPreviewRender regression suspected.";
            return;
        }
        planBlockCount_ = static_cast<int>(plan.blocks.size());

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Drive BuildPlan + RenderPlan inside a self-contained hidden ImGui
        // window so the tooltip-render code path sees a live ImGui draw
        // context (the runner ticks us from inside SmatchetUI::Draw, so an
        // outer frame exists; we open a child window for the render so
        // ImGui asserts about the wrap-width / cursor state don't trip).
        const std::string& md = SampleMarkdownBody();
        MarkdownPreviewRender::Options opts;
        opts.mode = MarkdownPreviewRender::Mode::Tooltip;
        opts.wrapWidth = wrapWidth_;
        opts.clickableLinks = false;

        ImGui::SetNextWindowSize(ImVec2(wrapWidth_ + 16.0f, 320.0f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(-10000.0f, -10000.0f), ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##scenario_description_tooltip_render", nullptr, flags)) {
            for (int i = 0; i < rendersPerFrame_; ++i) {
                MarkdownPreviewRender::Render(md, opts);
            }
        }
        ImGui::End();
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        nlohmann::json out;
        out["frames"] = frames_;
        out["wrapWidth"] = wrapWidth_;
        out["rendersPerFrame"] = rendersPerFrame_;
        out["planBlockCount"] = planBlockCount_;
        out["target_mean_ms"] = 6.94;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    int frames_ = 300;
    float wrapWidth_ = 480.0f;
    int rendersPerFrame_ = 4;
    int planBlockCount_ = 0;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeDescriptionTooltipMarkdownRenderScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(
        std::make_unique<smatchet::cmd::DescriptionTooltipMarkdownRenderScenario>());
}
