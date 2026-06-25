#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
#include "imgui.h"

#include <cmath>

// Mobile Phase-1 slice P1.6: pins the SmatchetDark (default theme) accent darken
// that fixes the WCAG AA white-on-fill contrast defect. The old accent
// (0.35,0.55,0.95) was used as the OPAQUE fill behind white(0.95) Text on
// ButtonActive / HeaderActive — white-on-fill 2.90:1, below the 4.5 AA-normal
// floor and even the 3.0 UI-component floor. A naive darken to the research
// doc's (0.20,0.34,0.62) fixed that but dropped the opaque on-dark glyphs
// (CheckMark / SliderGrab / SeparatorActive / NavHighlight, 1.0 alpha on the
// 0.12-luma WindowBg) to 2.35:1, failing the 3.0 floor. The chosen
// (0.26,0.42,0.72) is the single hue-preserving shade that clears BOTH:
//   white(0.95)-on-fill              >= 4.5  (AA-normal)
//   accent-on-WindowBg(0.12,.12,.14) >= 3.0  (UI-component floor)
// These CHECKs are the AGENTS.md "new palette token gets a WCAG-AA contrast
// CHECK where the token sits on a known bg" pin — so a future palette retune
// can't silently regress either constraint.

namespace {

struct ImGuiCtxFixture {
    ImGuiCtxFixture() : ctx_(ImGui::CreateContext()) {}
    ~ImGuiCtxFixture() {
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    ImGuiContext* ctx_;
};

// sRGB component -> linear (WCAG 2.x relative-luminance transfer fn).
float SrgbToLinear(float c) { return (c <= 0.03928f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float RelLuminance(const ImVec4& c) {
    return 0.2126f * SrgbToLinear(c.x) + 0.7152f * SrgbToLinear(c.y) + 0.0722f * SrgbToLinear(c.z);
}

// WCAG contrast ratio (L1+0.05)/(L2+0.05), brighter over darker. Alpha is
// ignored on purpose: these tokens are evaluated as the OPAQUE forms that
// actually render (ButtonActive fill / on-dark glyphs are 1.0 alpha).
float ContrastRatio(const ImVec4& a, const ImVec4& b) {
    const float la = RelLuminance(a);
    const float lb = RelLuminance(b);
    const float hi = la > lb ? la : lb;
    const float lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

bool ApproxEqRgb(const ImVec4& a, float r, float g, float b) {
    return std::fabs(a.x - r) <= 1e-5f && std::fabs(a.y - g) <= 1e-5f && std::fabs(a.z - b) <= 1e-5f;
}

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetDark accent literal is the AA-fixed shade") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const ImVec4* colors = ImGui::GetStyle().Colors;

    // Every slot that used the old (0.35,0.55,0.95) accent now carries the
    // darkened (0.26,0.42,0.72), each keeping its original alpha.
    CHECK(ApproxEqRgb(colors[ImGuiCol_CheckMark], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_SliderGrab], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_ButtonActive], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_HeaderActive], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_SeparatorActive], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_NavHighlight], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_TextSelectedBg], 0.26f, 0.42f, 0.72f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_DockingPreview], 0.26f, 0.42f, 0.72f));

    // SliderGrabActive stays one step brighter than SliderGrab (preserves the
    // original +0.10,+0.10,+0.05 active-step relationship).
    CHECK(ApproxEqRgb(colors[ImGuiCol_SliderGrabActive], 0.36f, 0.52f, 0.77f));
    CHECK(colors[ImGuiCol_SliderGrabActive].x > colors[ImGuiCol_SliderGrab].x);
    CHECK(colors[ImGuiCol_SliderGrabActive].y > colors[ImGuiCol_SliderGrab].y);
    CHECK(colors[ImGuiCol_SliderGrabActive].z > colors[ImGuiCol_SliderGrab].z);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetDark accent meets WCAG AA on white text AND the UI floor on dark") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const ImVec4* colors = ImGui::GetStyle().Colors;

    const ImVec4& text = colors[ImGuiCol_Text];         // (0.95,0.95,0.95)
    const ImVec4& windowBg = colors[ImGuiCol_WindowBg]; // (0.12,0.12,0.14)
    const ImVec4& accentFill = colors[ImGuiCol_ButtonActive];

    // Constraint (a): white(0.95) Text on the opaque accent fill >= 4.5:1 (AA-normal).
    const float whiteOnFill = ContrastRatio(text, accentFill);
    INFO("white-on-fill = ", whiteOnFill);
    CHECK(whiteOnFill >= 4.5f);

    // Constraint (b): opaque accent glyph on WindowBg >= 3.0:1 (UI-component floor).
    const float accentOnDark = ContrastRatio(accentFill, windowBg);
    INFO("accent-on-WindowBg = ", accentOnDark);
    CHECK(accentOnDark >= 3.0f);

    // Pin the actual measured values (with margin) so a regression is visible.
    CHECK(whiteOnFill == doctest::Approx(4.665f).epsilon(0.01f));
    CHECK(accentOnDark == doctest::Approx(3.163f).epsilon(0.01f));

    // Regression guard: the OLD accent (0.35,0.55,0.95) must NOT satisfy (a).
    const ImVec4 oldAccent(0.35f, 0.55f, 0.95f, 1.00f);
    CHECK(ContrastRatio(text, oldAccent) < 4.5f);
}

// ----- ModernDark (opt-in, non-default theme) — accent-contrast follow-up -----
// ModernDark had the SAME white-on-fill defect (old accent 0.45,0.65,0.95 →
// 2.12:1) but needs its OWN darker shade: its dimmer Text(0.92,0.93,0.95) lands
// the SmatchetDark shade (0.26,0.42,0.72) at only ~4.45:1 (<4.5). The shade
// (0.29,0.42,0.62) is the hue-faithful value clearing both ModernDark floors.

TEST_CASE_FIXTURE(ImGuiCtxFixture, "ModernDark accent literal is its own AA-fixed shade") {
    SmatchetTheme::ApplyStyle(ThemeId::ModernDark);
    const ImVec4* colors = ImGui::GetStyle().Colors;

    // Every slot that used the old (0.45,0.65,0.95) accent now carries the
    // ModernDark-specific (0.29,0.42,0.62), each keeping its original alpha.
    CHECK(ApproxEqRgb(colors[ImGuiCol_CheckMark], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_SliderGrab], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_ButtonActive], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_HeaderActive], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_SeparatorActive], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_NavHighlight], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_TextSelectedBg], 0.29f, 0.42f, 0.62f));
    CHECK(ApproxEqRgb(colors[ImGuiCol_DockingPreview], 0.29f, 0.42f, 0.62f));

    // SliderGrabActive keeps the one-step-brighter (+.10,+.10,+.05) relationship.
    CHECK(ApproxEqRgb(colors[ImGuiCol_SliderGrabActive], 0.39f, 0.52f, 0.67f));
    CHECK(colors[ImGuiCol_SliderGrabActive].x > colors[ImGuiCol_SliderGrab].x);
    CHECK(colors[ImGuiCol_SliderGrabActive].y > colors[ImGuiCol_SliderGrab].y);
    CHECK(colors[ImGuiCol_SliderGrabActive].z > colors[ImGuiCol_SliderGrab].z);

    // The two shades must be distinct — ModernDark is darker on blue than SmatchetDark.
    CHECK_FALSE(ApproxEqRgb(colors[ImGuiCol_ButtonActive], 0.26f, 0.42f, 0.72f));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "ModernDark accent meets WCAG AA on its dimmer text AND the UI floor on dark") {
    SmatchetTheme::ApplyStyle(ThemeId::ModernDark);
    const ImVec4* colors = ImGui::GetStyle().Colors;

    const ImVec4& text = colors[ImGuiCol_Text];         // (0.92,0.93,0.95) — dimmer than SmatchetDark's 0.95
    const ImVec4& windowBg = colors[ImGuiCol_WindowBg]; // (0.10,0.11,0.13)
    const ImVec4& accentFill = colors[ImGuiCol_ButtonActive];

    // Constraint (a): ModernDark Text on the opaque accent fill >= 4.5:1.
    const float whiteOnFill = ContrastRatio(text, accentFill);
    INFO("ModernDark white-on-fill = ", whiteOnFill);
    CHECK(whiteOnFill >= 4.5f);

    // Constraint (b): opaque accent glyph on WindowBg >= 3.0:1 (UI-component floor).
    const float accentOnDark = ContrastRatio(accentFill, windowBg);
    INFO("ModernDark accent-on-WindowBg = ", accentOnDark);
    CHECK(accentOnDark >= 3.0f);

    CHECK(whiteOnFill == doctest::Approx(4.610f).epsilon(0.01f));
    CHECK(accentOnDark == doctest::Approx(3.160f).epsilon(0.01f));

    // Regression guard 1: ModernDark's OLD accent (0.45,0.65,0.95) failed (a).
    const ImVec4 oldModernAccent(0.45f, 0.65f, 0.95f, 1.00f);
    CHECK(ContrastRatio(text, oldModernAccent) < 4.5f);

    // Regression guard 2: the SmatchetDark shade (0.26,0.42,0.72) is NOT reusable
    // on ModernDark's dimmer Text (~4.45:1 < 4.5) — this is WHY ModernDark needs
    // its own darker (0.29,0.42,0.62). Pins the per-theme-shade decision.
    const ImVec4 sharedDarkShade(0.26f, 0.42f, 0.72f, 1.00f);
    CHECK(ContrastRatio(text, sharedDarkShade) < 4.5f);
}
