// norton_commander_theme.test.cpp — bucket-E live-apply check for the
// NortonCommander theme. Doctest covers `SmatchetTheme::ApplyStyle()` in an
// isolated ImGui context; this bucket-E variant proves the apply path runs
// cleanly inside the live ImGuiContext owned by the running standalone app
// (real font, real platform, real frame loop) and writes the expected chrome
// colors into ImGui::GetStyle().Colors[ImGuiCol_*].
//
// What it asserts:
//   - WindowBg == deep blue ~(0, 0, 0.667, 1)
//   - TitleBg / MenuBarBg == cyan ~(0, 0.667, 0.667, 1)
//   - CheckMark == bright yellow ~(1, 1, 0.333, 1)
//   - All four channels of each above pass an absolute tolerance of 1e-4 vs
//     the literal in SmatchetTheme.cpp ApplyNortonCommander.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cmath>

namespace {

bool ChannelsApprox(const ImVec4& a, float r, float g, float b, float al) {
    const float eps = 1e-4f;
    return std::fabs(a.x - r) < eps && std::fabs(a.y - g) < eps && std::fabs(a.z - b) < eps &&
           std::fabs(a.w - al) < eps;
}

} // namespace

extern "C" void SmatchetRegisterNortonCommanderThemeTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Theme", "NortonCommander_LiveApply");

    // No GuiFunc needed — we only assert against ImGui::GetStyle() values that
    // ApplyStyle writes synchronously. The engine still ticks frames between
    // Yield() calls so the assertions run against post-apply state.
    t->GuiFunc = [](ImGuiTestContext*) {
        if (ImGui::Begin("SmatchetTest::NortonCommanderTheme")) {
            ImGui::TextUnformatted("Norton Commander palette probe");
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Capture the current palette so we can restore it after the assertion.
        // Failing to restore would leak the Norton blue into subsequent tests
        // running in the same engine session.
        const ImVec4 prevWindowBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

        SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
        ctx->Yield(); // let one frame run so the new palette is observable.

        const ImGuiStyle& s = ImGui::GetStyle();
        IM_CHECK(ChannelsApprox(s.Colors[ImGuiCol_WindowBg], 0.00f, 0.00f, 0.667f, 1.00f));
        IM_CHECK(ChannelsApprox(s.Colors[ImGuiCol_TitleBg], 0.00f, 0.667f, 0.667f, 1.00f));
        IM_CHECK(ChannelsApprox(s.Colors[ImGuiCol_MenuBarBg], 0.00f, 0.667f, 0.667f, 1.00f));
        IM_CHECK(ChannelsApprox(s.Colors[ImGuiCol_CheckMark], 1.00f, 1.00f, 0.333f, 1.00f));

        // Verify the C++ syntax-color side channel also flipped — the live ImGui
        // app reads gSyntaxColors via SmatchetTheme::GetSyntaxColors() to recolor
        // BlameAnalysisUi, so this proves both sides of the apply path fired.
        const SmatchetThemeSyntaxColors& syn = SmatchetTheme::GetSyntaxColors();
        IM_CHECK(std::fabs(syn.Keyword[0] - 1.000f) < 1e-4f);
        IM_CHECK(std::fabs(syn.Keyword[1] - 1.000f) < 1e-4f);
        IM_CHECK(std::fabs(syn.Keyword[2] - 0.333f) < 1e-4f);
        IM_CHECK(std::fabs(syn.Number[0] - 0.333f) < 1e-4f);
        IM_CHECK(std::fabs(syn.Number[1] - 1.000f) < 1e-4f);
        IM_CHECK(std::fabs(syn.Number[2] - 1.000f) < 1e-4f);

        // Restore prior palette so subsequent tests start from a known baseline.
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = prevWindowBg;
    };
}

#endif // SMATCHET_BUILD_UI_TESTS
