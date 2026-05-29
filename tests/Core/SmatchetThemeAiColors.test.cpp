#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
#include "imgui.h"

#include <cmath>

// Phase 5 of ai-chat-claude-desktop-parity. Pins the 6-token AI palette per
// theme so Phase 6 consumers (action row, bubble bg, pin strip) can rely on a
// stable shape. Mirrors the SmatchetThemeSyntaxColors.test.cpp pattern — same
// ImGuiCtxFixture lifetime, same channel-wise ApproxEq comparator.

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

bool ApproxEq(const float (&a)[4], const float (&b)[4]) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-5f) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes SmatchetDark AI palette") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const SmatchetThemeAiColors& a = SmatchetTheme::GetActiveAiColors();

    const float userBubbleBg[4] = {0.35f, 0.55f, 0.95f, 0.18f};
    const float userRoleLabel[4] = {0.65f, 0.80f, 1.00f, 1.00f};
    const float assistantRoleLabel[4] = {0.85f, 0.75f, 1.00f, 1.00f};
    const float actionRowIcon[4] = {0.50f, 0.50f, 0.55f, 1.00f};
    const float actionRowIconHover[4] = {0.95f, 0.95f, 0.95f, 1.00f};
    const float pinStripBg[4] = {0.16f, 0.16f, 0.20f, 0.90f};

    CHECK(ApproxEq(a.AiUserBubbleBg, userBubbleBg));
    CHECK(ApproxEq(a.AiUserRoleLabel, userRoleLabel));
    CHECK(ApproxEq(a.AiAssistantRoleLabel, assistantRoleLabel));
    CHECK(ApproxEq(a.AiActionRowIcon, actionRowIcon));
    CHECK(ApproxEq(a.AiActionRowIconHover, actionRowIconHover));
    CHECK(ApproxEq(a.AiPinStripBg, pinStripBg));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes Vs2022Light AI palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const SmatchetThemeAiColors& a = SmatchetTheme::GetActiveAiColors();

    // Light theme bumps bubble alpha to 0.20 vs the dark-themes' 0.18 since a
    // pure-white WindowBg dilutes the tint more at the same opacity. Role
    // labels are darker variants of accent / purple so they read on white.
    const float userBubbleBg[4] = {0.00f, 0.48f, 0.80f, 0.20f};
    const float userRoleLabel[4] = {0.00f, 0.38f, 0.65f, 1.00f};
    const float assistantRoleLabel[4] = {0.50f, 0.25f, 0.75f, 1.00f};
    const float actionRowIcon[4] = {0.50f, 0.50f, 0.50f, 1.00f};
    const float actionRowIconHover[4] = {0.10f, 0.10f, 0.10f, 1.00f};
    const float pinStripBg[4] = {0.92f, 0.93f, 0.95f, 0.95f};

    CHECK(ApproxEq(a.AiUserBubbleBg, userBubbleBg));
    CHECK(ApproxEq(a.AiUserRoleLabel, userRoleLabel));
    CHECK(ApproxEq(a.AiAssistantRoleLabel, assistantRoleLabel));
    CHECK(ApproxEq(a.AiActionRowIcon, actionRowIcon));
    CHECK(ApproxEq(a.AiActionRowIconHover, actionRowIconHover));
    CHECK(ApproxEq(a.AiPinStripBg, pinStripBg));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes HighContrast AI palette") {
    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const SmatchetThemeAiColors& a = SmatchetTheme::GetActiveAiColors();

    // HighContrast bumps bubble alpha to 0.35 — a pure-black WindowBg shows
    // little tint at low alpha, and the bubble must still read as a distinct
    // surface for low-vision users. Yellow / cyan primaries are AAA-contrast
    // on black.
    const float userBubbleBg[4] = {0.00f, 1.00f, 1.00f, 0.35f};
    const float userRoleLabel[4] = {1.00f, 1.00f, 0.00f, 1.00f};
    const float assistantRoleLabel[4] = {0.00f, 1.00f, 1.00f, 1.00f};
    const float actionRowIcon[4] = {1.00f, 1.00f, 1.00f, 1.00f};
    const float actionRowIconHover[4] = {1.00f, 1.00f, 0.00f, 1.00f};
    const float pinStripBg[4] = {0.00f, 0.00f, 0.00f, 1.00f};

    CHECK(ApproxEq(a.AiUserBubbleBg, userBubbleBg));
    CHECK(ApproxEq(a.AiUserRoleLabel, userRoleLabel));
    CHECK(ApproxEq(a.AiAssistantRoleLabel, assistantRoleLabel));
    CHECK(ApproxEq(a.AiActionRowIcon, actionRowIcon));
    CHECK(ApproxEq(a.AiActionRowIconHover, actionRowIconHover));
    CHECK(ApproxEq(a.AiPinStripBg, pinStripBg));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle AI bubble bg diverges across themes") {
    // Pairwise divergence proof: the AiUserBubbleBg literal must differ between
    // any two themes that aren't intentionally siblings. Catches a future copy-
    // paste bug where a new theme's apply function inherits another theme's
    // AI block by accident.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const float smatchetBubble[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const float vsLightBubble[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const float hcBubble[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const float ncBubble[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    CHECK_FALSE(ApproxEq(smatchetBubble, vsLightBubble));
    CHECK_FALSE(ApproxEq(smatchetBubble, hcBubble));
    CHECK_FALSE(ApproxEq(smatchetBubble, ncBubble));
    CHECK_FALSE(ApproxEq(vsLightBubble, hcBubble));
    CHECK_FALSE(ApproxEq(vsLightBubble, ncBubble));
    CHECK_FALSE(ApproxEq(hcBubble, ncBubble));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle AI palette is idempotent") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float first[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float second[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    CHECK(ApproxEq(first, second));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle AI palette round-trips through every theme") {
    // Round-trip proof: SmatchetDark → other theme → SmatchetDark must restore
    // every AI token. Catches accidental leak via a future PushStyleColor-style
    // bug on a per-theme branch.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const float baselineBubble[4] = {
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
        SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};

    const ThemeId others[] = {ThemeId::ModernDark,   ThemeId::Vs2022Dark,      ThemeId::Vs2022Light,
                              ThemeId::HighContrast, ThemeId::NortonCommander, ThemeId::ImGuiDefaultDark};
    for (ThemeId t : others) {
        SmatchetTheme::ApplyStyle(t);
        SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
        const float restoredBubble[4] = {
            SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[0], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[1],
            SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[2], SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3]};
        INFO("Detour theme=", static_cast<int>(t));
        CHECK(ApproxEq(baselineBubble, restoredBubble));
    }
}

// Pillar 4 aspirational floor: bubble bg alpha must be non-zero AND less than
// or equal to 0.5 on every theme. Below 0 = invisible (bubble doesn't render);
// above 0.5 = bubble dominates the text. Doesn't validate WCAG AA contrast
// numerically (that lives in a future a11y audit), but pins the alpha-band
// invariant so a future palette retune can't accidentally ship a bubble that
// either disappears or drowns out the body text.
TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle AI bubble alpha stays in [0.10, 0.50] on every theme") {
    const ThemeId every[] = {ThemeId::SmatchetDark,    ThemeId::ModernDark,   ThemeId::Vs2022Dark,
                             ThemeId::Vs2022Light,     ThemeId::HighContrast, ThemeId::NortonCommander,
                             ThemeId::ImGuiDefaultDark};
    for (ThemeId t : every) {
        SmatchetTheme::ApplyStyle(t);
        const float alpha = SmatchetTheme::GetActiveAiColors().AiUserBubbleBg[3];
        INFO("Theme=", static_cast<int>(t), " bubble alpha=", alpha);
        CHECK(alpha >= 0.10f);
        CHECK(alpha <= 0.50f);
    }
}
