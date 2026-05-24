// description_tooltip_markdown_render.test.cpp — bucket-E coverage for the
// grid-cell description tooltip that renders markdown with a fixed wrap
// width. Closes the defensive cover for the be2b1d9 wrapWidth-tooltip-render
// regression (the original bug caused unset wrapWidth → tooltip rendered
// edge-to-edge at content-region default; the test asserts the tooltip
// path honours the configured value).
//
// Drift warning — IF YOU CHANGE Source_Core/src/TicketFieldEditor.cpp:596
// (the `opts.wrapWidth = ImGui::GetFontSize() * 48.0f;` line, or its sibling
// at :1096 for the body preview), Source_Core/include/MarkdownPreviewRender.h:115
// (the Options::wrapWidth field), OR Source_Core/src/MarkdownPreviewRender.cpp:996
// (where Options::wrapWidth maps to PreviewPlan::fixedWrapWidth via RenderPlan),
// UPDATE THIS TEST to match.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "MarkdownPreviewRender.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

namespace {

struct TooltipRenderState {
    std::string markdown;
    bool drewTooltip = false;
    int planBlockCount = 0;
    float configuredWrap = 0.0f;
};

TooltipRenderState g_tooltipRenderState;

void ResetTooltipRenderState() {
    g_tooltipRenderState.markdown.clear();
    g_tooltipRenderState.drewTooltip = false;
    g_tooltipRenderState.planBlockCount = 0;
    g_tooltipRenderState.configuredWrap = 0.0f;
}

// Mirrors TicketFieldEditor.cpp:596 — fixed wrap of 48 ems for the description
// tooltip path so the tooltip renders at a stable readable width regardless
// of grid-column width.
constexpr float kEmsForDescriptionTooltip = 48.0f;

void DrawDescriptionTooltipReplica(TooltipRenderState& s) {
    ImGui::SetNextWindowSize(ImVec2(640, 200), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::DescriptionTooltip", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Button("Hoverable##DT");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            MarkdownPreviewRender::Options opts;
            opts.mode = MarkdownPreviewRender::Mode::Tooltip;
            opts.wrapWidth = ImGui::GetFontSize() * kEmsForDescriptionTooltip;
            opts.clickableLinks = false;
            s.configuredWrap = opts.wrapWidth;
            MarkdownPreviewRender::PreviewPlan plan;
            MarkdownPreviewRender::BuildPlan(s.markdown, plan);
            s.planBlockCount = static_cast<int>(plan.blocks.size());
            MarkdownPreviewRender::RenderPlan(plan, opts);
            ImGui::EndTooltip();
            s.drewTooltip = true;
        }
    }
    ImGui::End();
}

void RegisterDescriptionTooltipVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DescriptionTooltip", "MarkdownRender_WrapWidthHonoured");
    t->UserData = &g_tooltipRenderState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TooltipRenderState*>(ctx->Test->UserData);
        DrawDescriptionTooltipReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TooltipRenderState*>(ctx->Test->UserData);
        ResetTooltipRenderState();
        s->markdown = "# Heading\n\nFirst paragraph with **bold** and *italic* runs that exceed a "
                      "single-line width to force the wrap path to kick in.\n\n"
                      "- list item one\n- list item two\n";
        ctx->SetRef("SmatchetTest::DescriptionTooltip");
        ctx->Yield();
        ctx->Yield();

        ctx->MouseMove("Hoverable##DT");
        // Tooltip needs at least 1 frame to materialise after hover.
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(s->drewTooltip);
        IM_CHECK(s->planBlockCount > 0);
        // wrapWidth must be the configured 48 ems, not the default 0.0f
        // (which would collapse to ContentRegionAvail.x at render time — the
        // exact regression be2b1d9 fixed).
        IM_CHECK(s->configuredWrap > 0.0f);
        const float fontSize = ImGui::GetFontSize();
        IM_CHECK_EQ(s->configuredWrap, fontSize * kEmsForDescriptionTooltip);
    };
}

} // namespace

extern "C" void SmatchetRegisterDescriptionTooltipMarkdownRenderTests(ImGuiTestEngine* engine) {
    RegisterDescriptionTooltipVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
