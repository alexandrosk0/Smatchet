#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
#include "SmatchetUiDensity.h"
#include "imgui.h"

#include <cmath>

namespace {

// Per-frame ImGui style lives in a global state owned by the active context. ApplyStyle()
// dereferences ImGui::GetStyle(), so every test_case must build a context, run the apply, then
// destroy. RAII fixture keeps the lifetime tight and re-entrant across SUBCASE.
struct ImGuiCtxFixture {
    ImGuiCtxFixture() : ctx_(ImGui::CreateContext()) {}
    ~ImGuiCtxFixture() {
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    ImGuiContext* ctx_;
};

// Channel-wise byte equality is overkill for floats stored as 0..1; doctest's default tolerance
// matches against floats but the palette literals in SmatchetTheme.cpp are also stored as floats,
// so equality is safe for hard-coded constants. Helper exists to keep CHECK lines readable.
bool ApproxEq(const float (&a)[4], const float (&b)[4]) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-5f) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes SmatchetDark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    const float keyword[4] = {0.78f, 0.50f, 1.00f, 1.0f};
    const float strLit[4] = {0.95f, 0.65f, 0.45f, 1.0f};
    const float comment[4] = {0.45f, 0.75f, 0.45f, 1.0f};
    const float number[4] = {0.65f, 0.85f, 1.00f, 1.0f};
    const float preproc[4] = {0.85f, 0.85f, 0.50f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes ModernDark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::ModernDark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // ModernDark shares the SmatchetDark syntax family (only chrome diverges) — encoded explicitly
    // so a future divergence between the two trips the assertion instead of silently passing.
    const float keyword[4] = {0.78f, 0.50f, 1.00f, 1.0f};
    CHECK(ApproxEq(s.Keyword, keyword));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes Vs2022Dark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // VS Code Dark+ canonical: keyword #569CD6, string #CE9178, comment #6A9955, number #B5CEA8,
    // preproc #9B9B9B. Source-of-truth lives in SmatchetTheme.cpp ApplyVs2022Dark.
    const float keyword[4] = {0.34f, 0.61f, 0.84f, 1.0f};
    const float strLit[4] = {0.81f, 0.57f, 0.47f, 1.0f};
    const float comment[4] = {0.42f, 0.60f, 0.33f, 1.0f};
    const float number[4] = {0.71f, 0.81f, 0.66f, 1.0f};
    const float preproc[4] = {0.61f, 0.61f, 0.61f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes Vs2022Light syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // VS Code Light+ canonical: keyword #0000FF (pure blue), string #A31515, comment #008000,
    // number #098658, preproc #808080. Chosen for legibility on white WindowBg.
    const float keyword[4] = {0.00f, 0.00f, 1.00f, 1.0f};
    const float strLit[4] = {0.64f, 0.08f, 0.08f, 1.0f};
    const float comment[4] = {0.00f, 0.50f, 0.00f, 1.0f};
    const float number[4] = {0.04f, 0.53f, 0.35f, 1.0f};
    const float preproc[4] = {0.50f, 0.50f, 0.50f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes HighContrast syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // Fully saturated primaries — keyword yellow, string magenta, comment green, number cyan,
    // preproc orange. Picked for low-vision / accessibility audits on pure-black background.
    const float keyword[4] = {1.00f, 1.00f, 0.00f, 1.0f};
    const float strLit[4] = {1.00f, 0.00f, 1.00f, 1.0f};
    const float comment[4] = {0.00f, 1.00f, 0.00f, 1.0f};
    const float number[4] = {0.00f, 1.00f, 1.00f, 1.0f};
    const float preproc[4] = {1.00f, 0.65f, 0.00f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes NortonCommander syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // Norton Commander DOS tribute — keyword bright yellow #FFFF55, string light red, comment light
    // gray, number bright cyan #55FFFF, preproc bright green. Source-of-truth in SmatchetTheme.cpp
    // ApplyNortonCommander.
    const float keyword[4] = {1.00f, 1.00f, 0.333f, 1.0f};
    const float strLit[4] = {1.00f, 0.50f, 0.50f, 1.0f};
    const float comment[4] = {0.667f, 0.667f, 0.667f, 1.0f};
    const float number[4] = {0.333f, 1.00f, 1.00f, 1.0f};
    const float preproc[4] = {0.333f, 1.00f, 0.333f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle keyword color diverges between themes") {
    // Round-trip proof for the user-facing claim — switching theme actually recolors the syntax
    // palette in the next frame. Captures the SmatchetDark / Vs2022Dark / Vs2022Light /
    // HighContrast keyword channel and asserts pairwise inequality across the families. ModernDark
    // is intentionally not in this check because it currently shares SmatchetDark's syntax family.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const float smatchetKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float vsDarkKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const float vsLightKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const float highContrastKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const float nortonKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    CHECK_FALSE(ApproxEq(smatchetKeyword, vsDarkKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(vsLightKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsLightKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(highContrastKeyword, nortonKeyword));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle is idempotent for syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float first[4] = {SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
                            SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float second[4] = {SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
                             SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    CHECK(ApproxEq(first, second));
}

namespace {
struct StyleSnapshot {
    ImVec4 colors[ImGuiCol_COUNT];
    float WindowRounding;
    float ChildRounding;
    float FrameRounding;
    float PopupRounding;
    float ScrollbarRounding;
    float GrabRounding;
    float TabRounding;
    ImVec2 WindowPadding;
    ImVec2 FramePadding;
    ImVec2 ItemSpacing;
    float ScrollbarSize;
};

StyleSnapshot SnapshotStyle() {
    const ImGuiStyle& s = ImGui::GetStyle();
    StyleSnapshot out;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        out.colors[i] = s.Colors[i];
    }
    out.WindowRounding = s.WindowRounding;
    out.ChildRounding = s.ChildRounding;
    out.FrameRounding = s.FrameRounding;
    out.PopupRounding = s.PopupRounding;
    out.ScrollbarRounding = s.ScrollbarRounding;
    out.GrabRounding = s.GrabRounding;
    out.TabRounding = s.TabRounding;
    out.WindowPadding = s.WindowPadding;
    out.FramePadding = s.FramePadding;
    out.ItemSpacing = s.ItemSpacing;
    out.ScrollbarSize = s.ScrollbarSize;
    return out;
}

bool Vec4Eq(const ImVec4& a, const ImVec4& b) {
    return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f &&
           std::fabs(a.w - b.w) < 1e-5f;
}

bool Vec2Eq(const ImVec2& a, const ImVec2& b) { return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f; }
} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture,
                  "SmatchetTheme::ApplyStyle round-trips full ImGuiStyle for SmatchetDark<->NortonCommander") {
    // Mimic the host's boot sequence (SmatchetImGuiHost.cpp:449-451): StyleColorsDark first, then
    // ApplyStyle. The round-trip path must reach the same state without re-running StyleColorsDark.
    ImGui::StyleColorsDark();
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot before = SnapshotStyle();

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);

    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot after = SnapshotStyle();

    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        INFO("ImGuiCol_ index ", i);
        CHECK(Vec4Eq(before.colors[i], after.colors[i]));
    }
    CHECK(before.WindowRounding == after.WindowRounding);
    CHECK(before.ChildRounding == after.ChildRounding);
    CHECK(before.FrameRounding == after.FrameRounding);
    CHECK(before.PopupRounding == after.PopupRounding);
    CHECK(before.ScrollbarRounding == after.ScrollbarRounding);
    CHECK(before.GrabRounding == after.GrabRounding);
    CHECK(before.TabRounding == after.TabRounding);
    CHECK(Vec2Eq(before.WindowPadding, after.WindowPadding));
    CHECK(Vec2Eq(before.FramePadding, after.FramePadding));
    CHECK(Vec2Eq(before.ItemSpacing, after.ItemSpacing));
    CHECK(before.ScrollbarSize == after.ScrollbarSize);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle round-trips full ImGuiStyle through every theme") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot before = SnapshotStyle();

    const ThemeId others[] = {ThemeId::ModernDark, ThemeId::Vs2022Dark, ThemeId::Vs2022Light, ThemeId::HighContrast,
                              ThemeId::NortonCommander};
    for (ThemeId t : others) {
        SmatchetTheme::ApplyStyle(t);
        SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
        const StyleSnapshot after = SnapshotStyle();
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            INFO("Detour theme=", static_cast<int>(t), " ImGuiCol_ index=", i);
            CHECK(Vec4Eq(before.colors[i], after.colors[i]));
        }
    }
}

// Density-preservation policy: SmatchetUI re-runs ApplyDensityToImGuiStyle after every
// SmatchetTheme::ApplyStyle. The test pins the policy — push density, apply a theme, re-push
// density, and assert the user's spacing survives. Without the re-push, ApplyCommonStyle
// reverts ItemSpacing / FramePadding to Normal defaults silently.
TEST_CASE_FIXTURE(ImGuiCtxFixture, "Density survives theme switch when re-applied after ApplyStyle") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    const ImVec2 compactItem = ImGui::GetStyle().ItemSpacing;
    const ImVec2 compactFrame = ImGui::GetStyle().FramePadding;
    CHECK(Vec2Eq(compactItem, ImVec2(4.0f, 2.0f)));
    CHECK(Vec2Eq(compactFrame, ImVec2(4.0f, 2.0f)));

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    // Right after a raw ApplyStyle the spacing reverts to Normal density — that is the regression
    // SmatchetUI guards against by re-pushing density immediately after.
    CHECK_FALSE(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, compactFrame));

    // Round-trip back to the original theme also preserves density once re-pushed.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, compactFrame));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "ApplyDensityToImGuiStyle writes the documented constants") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(4.0f, 2.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(4.0f, 2.0f)));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Comfortable);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(10.0f, 8.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(8.0f, 6.0f)));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Normal);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(8.0f, 6.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(6.0f, 4.0f)));
}
