#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
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

    CHECK_FALSE(ApproxEq(smatchetKeyword, vsDarkKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsLightKeyword, highContrastKeyword));
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
