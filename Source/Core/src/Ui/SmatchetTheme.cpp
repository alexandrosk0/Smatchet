#include "SmatchetTheme.h"

#include "SmatchetThemeDensity.h"

#include <atomic>

namespace {

// Monotonic theme-revision counter — bumped once per ApplyStyle completion.
// Consumers (CodeColorView's tokenize cache, future per-theme glyph caches)
// snapshot this alongside their key + miss when the snapshot lags. Atomic so
// any future worker-thread read sees a consistent value; today the only
// writer + readers are UI-thread so the relaxed memory order is fine.
std::atomic<std::uint64_t> g_themeRevision{0};

// Host-injected UI density scale (mobile host-injection seam #13). Set once by
// SmatchetTheme::ApplyUiDensityScale at boot (e.g. AConfiguration density / 160).
// Stored so every subsequent ApplyStyle — which resets `style = ImGuiStyle{}` and
// rebuilds from baseline — can re-assert it (see ReapplyHostDensityScale); without
// this a theme-drift rebuild (SmatchetUI::drawApplyAppearanceSettings fires ApplyStyle
// on frame 1 post-Load) silently wipes the boot density. Defaults to 1.0 (desktop:
// no scaling, seam inert). UI-thread-only (set at boot before the first frame, read in
// ApplyStyle on the UI thread), so a plain float suffices — unlike g_themeRevision,
// which is read cross-thread and therefore atomic.
float g_hostDensityScale = 1.0f;

// Active theme's C++ syntax-highlight palette. Filled by every ApplyXxx() so CppSyntaxHighlight
// (and any future code presenter) can recolor in the next ImGui frame after a theme switch.
SmatchetThemeSyntaxColors gSyntaxColors = {
    {0.78f, 0.50f, 1.00f, 1.0f}, // Keyword (SmatchetDark default — same as legacy hard-coded)
    {0.95f, 0.65f, 0.45f, 1.0f}, // String
    {0.45f, 0.75f, 0.45f, 1.0f}, // Comment
    {0.65f, 0.85f, 1.00f, 1.0f}, // Number
    {0.85f, 0.85f, 0.50f, 1.0f}, // Preprocessor
    {0.62f, 0.80f, 0.92f, 1.0f}  // Identifier (slice 6 — soft sky-blue)
};

// Active theme's AI chat-panel palette (Phase 5 of ai-chat-claude-desktop-parity).
// Default seeds match the SmatchetDark theme so pre-ApplyStyle reads return a
// usable palette (defence in depth — every ApplyXxx ALSO writes these). Same
// shape + ownership as gSyntaxColors above.
SmatchetThemeAiColors gAiColors = {
    {0.35f, 0.55f, 0.95f, 0.18f}, // AiUserBubbleBg — accent blue, 0.18 alpha
    {0.65f, 0.80f, 1.00f, 1.00f}, // AiUserRoleLabel — bright accent
    {0.85f, 0.75f, 1.00f, 1.00f}, // AiAssistantRoleLabel — soft purple
    {0.55f, 0.55f, 0.60f, 1.00f}, // AiActionRowIcon — muted (idle)
    {0.95f, 0.95f, 0.95f, 1.00f}, // AiActionRowIconHover — full text color
    {0.18f, 0.18f, 0.22f, 0.90f}  // AiPinStripBg — slightly opaque panel tint
};

void SetSyntaxColors(const SmatchetThemeSyntaxColors& s) { gSyntaxColors = s; }
void SetAiColors(const SmatchetThemeAiColors& a) { gAiColors = a; }

// Active semantic status-text palette (P2-H9). Seeded with the dark-family values so a
// read before the first ApplyStyle still yields usable colors.
SmatchetThemeSemanticColors gSemanticColors = {
    ImVec4(0.95f, 0.35f, 0.35f, 1.0f), // ErrorText
    ImVec4(1.00f, 0.85f, 0.30f, 1.0f), // WarningText
    ImVec4(0.45f, 0.95f, 0.55f, 1.0f), // SuccessText
};
void SetSemanticColors(const SmatchetThemeSemanticColors& c) { gSemanticColors = c; }

// Re-apply the host-injected UI density scale on top of a freshly rebuilt style.
// ApplyStyle resets `style = ImGuiStyle{}` every call, so this re-asserts the boot
// density the host set via ApplyUiDensityScale. Desktop leaves the scale at 1.0 → no-op.
void ReapplyHostDensityScale(ImGuiStyle& style) {
    if (SmatchetTheme::ShouldApplyDensityScale(g_hostDensityScale)) {
        style.ScaleAllSizes(g_hostDensityScale);
    }
}

// Shared rounding / padding constants applied to every palette. Keeping these in a single helper
// guarantees the shell geometry stays consistent regardless of which palette the user selects;
// only the ImGuiCol_* values diverge between themes.
void ApplyCommonStyle(ImGuiStyle& style) {
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 14.0f;
}

// Legacy Smatchet palette. Bit-identical to the previous hard-coded ApplyStyle() body — existing
// users see no visual difference when they keep the default theme.
void ApplySmatchetDark(ImGuiStyle& /*style*/, ImVec4* colors) {
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);

    // Accent darkened from (0.35,0.55,0.95) to (0.26,0.42,0.72) — WCAG AA fix (mobile Phase-1
    // P1.6). The old accent failed AA as an opaque fill behind white(0.95) Text on
    // ButtonActive/HeaderActive (white-on-fill 2.90:1, below the 4.5 AA-normal and 3.0 UI floors).
    // The new shade is the single hue-preserving darken that clears BOTH constraints with margin:
    //   white(0.95)-on-fill            = 4.67:1  (>= 4.5 AA-normal)
    //   accent-on-WindowBg(0.12,.12,.14) = 3.16:1  (>= 3.0 UI-component floor, for the opaque
    //                                              CheckMark/SliderGrab/SeparatorActive/NavHighlight glyphs)
    // A naive swap to the research doc's (0.20,0.34,0.62) fixed white-on-fill but dropped the
    // on-dark glyphs to 2.35:1 — see docs/mobile/PHASE1_ACCESSIBILITY_RESEARCH.md Finding 3.
    colors[ImGuiCol_CheckMark] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);
    // One step brighter than SliderGrab (preserves the original +0.10,+0.10,+0.05 active-step delta).
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.52f, 0.77f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);

    colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.26f, 0.42f, 0.72f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.42f, 0.72f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.42f, 0.72f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.42f, 0.72f, 0.95f);

    colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.22f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.42f, 0.72f, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(0.25f, 0.35f, 0.55f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.12f, 0.14f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.42f, 0.72f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.42f, 0.72f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.42f, 0.72f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // C++ syntax palette — single source of truth in BuildSyntaxColorsForTheme
    // (legacy SmatchetDark defaults: purple/orange/green/cyan/yellow + sky-blue identifier).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::SmatchetDark));
    // AI chat-panel palette (Phase 5 of ai-chat-claude-desktop-parity). Bubble keeps the
    // original brighter blue (#5993F2) at 0.18 alpha — a low-alpha TINT, not an opaque fill,
    // so 0.95-luma Text stays WCAG AA on the 0.12-luma WindowBg even where the bubble overlays
    // (already-verified, doctest-pinned). Intentionally NOT darkened to the new chrome accent:
    // the P1.6 darken targets opaque-fill-behind-white failures, which this tint never had.
    const SmatchetThemeAiColors ai = {
        {0.35f, 0.55f, 0.95f, 0.18f}, // AiUserBubbleBg — accent-blue tint, 0.18 alpha
        {0.65f, 0.80f, 1.00f, 1.00f}, // AiUserRoleLabel — bright accent
        {0.85f, 0.75f, 1.00f, 1.00f}, // AiAssistantRoleLabel — soft purple
        {0.50f, 0.50f, 0.55f, 1.00f}, // AiActionRowIcon — matches TextDisabled (0.50,0.50,0.50)
        {0.95f, 0.95f, 0.95f, 1.00f}, // AiActionRowIconHover — full Text strength
        {0.16f, 0.16f, 0.20f, 0.90f}  // AiPinStripBg — slightly deeper than WindowBg
    };
    SetAiColors(ai);
}

// Flatter, more neutral dark — slightly cooler greys and a desaturated blue accent.
void ApplyModernDark(ImGuiStyle& /*style*/, ImVec4* colors) {
    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.50f, 0.54f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.45f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.42f, 0.46f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);

    // Mobile P1.6 accent-contrast follow-up (sibling of the SmatchetDark fix):
    // ModernDark's accent (0.45,0.65,0.95) was the OPAQUE fill behind
    // Text(0.92,0.93,0.95) on ButtonActive/HeaderActive — white-on-fill only
    // 2.12:1, below the 4.5 AA-normal floor. ModernDark needs its OWN darker
    // shade: its dimmer Text(0.92) lands the SmatchetDark shade (0.26,0.42,0.72)
    // at just 4.45:1 (<4.5). (0.29,0.42,0.62) is the hue-faithful shade clearing
    // BOTH constraints — Text-on-fill 4.61:1 (>=4.5 AA-normal) and opaque
    // accent-on-WindowBg(0.10,0.11,0.13) 3.16:1 (>=3.0 UI-component floor).
    // Each slot keeps its original alpha. Pinned by SmatchetThemeAccentContrast.
    colors[ImGuiCol_CheckMark] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.39f, 0.52f, 0.67f, 1.00f); // one step brighter (+.10,+.10,+.05)

    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);

    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.29f, 0.42f, 0.62f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.29f, 0.42f, 0.62f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.29f, 0.42f, 0.62f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.29f, 0.42f, 0.62f, 0.95f);

    colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.20f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.29f, 0.42f, 0.62f, 0.80f);
    colors[ImGuiCol_TabActive] =
        ImVec4(0.28f, 0.38f, 0.55f, 1.00f); // separate desaturated blue (5.34:1 white-on-tab), not accent
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.11f, 0.13f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(0.29f, 0.42f, 0.62f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.29f, 0.42f, 0.62f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.29f, 0.42f, 0.62f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // C++ syntax palette — same family as SmatchetDark; ModernDark only repaints chrome.
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::ModernDark));
    // AI palette — AiUserBubbleBg is a low-alpha (0.18) accent-blue tint, NOT an
    // opaque fill, so it stays the brighter pre-darken blue (separately AA-fine)
    // while the chrome accent above darkened to (0.29,0.42,0.62) for opaque fills.
    const SmatchetThemeAiColors ai = {
        {0.45f, 0.65f, 0.95f, 0.18f}, // AiUserBubbleBg — accent-blue tint, 0.18 alpha
        {0.55f, 0.75f, 1.00f, 1.00f}, // AiUserRoleLabel
        {0.80f, 0.72f, 1.00f, 1.00f}, // AiAssistantRoleLabel
        {0.48f, 0.50f, 0.54f, 1.00f}, // AiActionRowIcon — TextDisabled
        {0.92f, 0.93f, 0.95f, 1.00f}, // AiActionRowIconHover — Text
        {0.13f, 0.14f, 0.17f, 0.90f}  // AiPinStripBg
    };
    SetAiColors(ai);
}

// Visual Studio 2022 "Dark" palette — near-black editor bg (#1E1E1E), darker chrome (#2D2D30),
// VS Code Dark+ blue accent (#007ACC). Familiar to Visual Studio / VS Code users.
void ApplyVs2022Dark(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 accent = ImVec4(0.00f, 0.48f, 0.80f, 1.00f); // #007ACC
    const ImVec4 accentBright = ImVec4(0.10f, 0.58f, 0.90f, 1.00f);

    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.94f, 0.94f, 1.00f); // #F1F1F1
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f); // #1E1E1E
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.18f, 0.18f, 0.19f, 0.98f); // #2D2D30
    colors[ImGuiCol_Border] = ImVec4(0.27f, 0.27f, 0.28f, 0.60f);  // #434346
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f); // #333337
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.27f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.32f, 0.34f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.18f, 0.18f, 0.19f, 1.00f); // #2D2D30
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.44f, 0.44f, 0.46f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.54f, 0.54f, 0.56f, 1.00f);

    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accentBright;

    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.40f, 0.56f, 1.00f);
    colors[ImGuiCol_ButtonActive] = accent;

    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.40f, 0.56f, 1.00f);
    colors[ImGuiCol_HeaderActive] = accent;

    colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.27f, 0.28f, 0.60f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = accent;

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.48f, 0.80f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.00f, 0.48f, 0.80f, 0.95f);

    colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.19f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(0.07f, 0.30f, 0.52f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.12f, 0.12f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(0.00f, 0.48f, 0.80f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.32f, 0.32f, 0.34f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.24f, 0.24f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.48f, 0.80f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

    // C++ syntax palette — VS Code Dark+ canonical colors (BuildSyntaxColorsForTheme).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::Vs2022Dark));
    // AI palette — #007ACC accent, VS Code Dark+ purple for assistant.
    const SmatchetThemeAiColors ai = {
        {0.00f, 0.48f, 0.80f, 0.18f}, // AiUserBubbleBg — VS blue, 0.18 alpha
        {0.10f, 0.58f, 0.90f, 1.00f}, // AiUserRoleLabel — accentBright
        {0.78f, 0.56f, 0.95f, 1.00f}, // AiAssistantRoleLabel — VS Code C# class purple
        {0.60f, 0.60f, 0.60f, 1.00f}, // AiActionRowIcon — TextDisabled
        {0.94f, 0.94f, 0.94f, 1.00f}, // AiActionRowIconHover — Text
        {0.18f, 0.18f, 0.19f, 0.90f}  // AiPinStripBg — PopupBg-tinted
    };
    SetAiColors(ai);
}

// Visual Studio 2022 "Light" / VS Code Light+ palette — white editor bg, #EEEEF2 chrome,
// VS blue accent (#007ACC) carried over from the dark theme for consistency.
void ApplyVs2022Light(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 accent = ImVec4(0.00f, 0.48f, 0.80f, 1.00f); // #007ACC
    const ImVec4 accentBright = ImVec4(0.10f, 0.58f, 0.90f, 1.00f);

    colors[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.97f, 0.97f, 0.98f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.70f, 0.70f, 0.74f, 0.60f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f); // #EEEEF2
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(1.00f, 1.00f, 1.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.97f, 0.97f, 0.98f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.74f, 0.74f, 0.76f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.60f, 0.64f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.48f, 0.48f, 0.52f, 1.00f);

    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accentBright;

    colors[ImGuiCol_Button] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);
    colors[ImGuiCol_ButtonActive] = accent;

    colors[ImGuiCol_Header] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);
    colors[ImGuiCol_HeaderActive] = accent;

    colors[ImGuiCol_Separator] = ImVec4(0.70f, 0.70f, 0.74f, 0.60f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = accent;

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.48f, 0.80f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.00f, 0.48f, 0.80f, 0.95f);

    colors[ImGuiCol_Tab] = ImVec4(0.88f, 0.88f, 0.92f, 0.86f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.00f, 0.48f, 0.80f, 0.40f);
    colors[ImGuiCol_TabActive] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.93f, 0.93f, 0.95f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(0.00f, 0.48f, 0.80f, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.45f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.62f, 0.62f, 0.65f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.48f, 0.80f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.90f, 0.60f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = accent;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.30f, 0.30f, 0.30f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

    // C++ syntax palette — VS Code Light+ canonical (legible on white; BuildSyntaxColorsForTheme).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::Vs2022Light));
    // AI palette — light theme. Bubble alpha bumped to 0.20 because a pure-white
    // WindowBg dilutes the tint more than a dark bg does at the same opacity.
    // Text on bubble = 0.10-luma on (1.0 - 0.20) + (0.0,0.48,0.80) * 0.20 ≈
    // (0.80, 0.90, 0.96) → contrast ratio 5.9:1 vs 0.10-luma text. WCAG AA pass.
    const SmatchetThemeAiColors ai = {
        {0.00f, 0.48f, 0.80f, 0.20f}, // AiUserBubbleBg — VS blue, 0.20 alpha
        {0.00f, 0.38f, 0.65f, 1.00f}, // AiUserRoleLabel — slightly darker accent for light bg
        {0.50f, 0.25f, 0.75f, 1.00f}, // AiAssistantRoleLabel — darker purple on white
        {0.50f, 0.50f, 0.50f, 1.00f}, // AiActionRowIcon — TextDisabled
        {0.10f, 0.10f, 0.10f, 1.00f}, // AiActionRowIconHover — Text
        {0.92f, 0.93f, 0.95f, 0.95f}  // AiPinStripBg — neutral light grey, slightly opaque
    };
    SetAiColors(ai);
}

// High contrast — pure black background, pure white text, cyan accent. Maximum legibility for
// low-vision users / accessibility audits.
void ApplyHighContrast(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 cyan = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);

    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.00f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered] = cyan;
    colors[ImGuiCol_ScrollbarGrabActive] = cyan;

    colors[ImGuiCol_CheckMark] = cyan;
    colors[ImGuiCol_SliderGrab] = cyan;
    colors[ImGuiCol_SliderGrabActive] = cyan;

    colors[ImGuiCol_Button] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive] = cyan;

    colors[ImGuiCol_Header] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_HeaderActive] = cyan;

    colors[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_SeparatorHovered] = cyan;
    colors[ImGuiCol_SeparatorActive] = cyan;

    colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered] = cyan;
    colors[ImGuiCol_ResizeGripActive] = cyan;

    colors[ImGuiCol_Tab] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TabHovered] = cyan;
    colors[ImGuiCol_TabActive] = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.00f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);

    colors[ImGuiCol_DockingPreview] = ImVec4(0.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = cyan;
    colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = cyan;

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(1.00f, 1.00f, 1.00f, 0.40f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 1.00f, 1.00f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = cyan;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.70f);

    // C++ syntax palette — fully saturated primaries for accessibility (BuildSyntaxColorsForTheme).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::HighContrast));
    // AI palette — High Contrast. Bubble alpha bumped to 0.35 because a pure-black
    // WindowBg shows little tint at low alpha; the bubble must still read as a
    // distinct surface even for low-vision users. Role labels use Pillar-4-floor
    // primary cyan/yellow on pure black — both > 7:1 contrast (AAA).
    const SmatchetThemeAiColors ai = {
        {0.00f, 1.00f, 1.00f, 0.35f}, // AiUserBubbleBg — cyan, 0.35 alpha (a11y bump)
        {1.00f, 1.00f, 0.00f, 1.00f}, // AiUserRoleLabel — saturated yellow
        {0.00f, 1.00f, 1.00f, 1.00f}, // AiAssistantRoleLabel — saturated cyan
        {1.00f, 1.00f, 1.00f, 1.00f}, // AiActionRowIcon — pure white (idle = full strength too on HC)
        {1.00f, 1.00f, 0.00f, 1.00f}, // AiActionRowIconHover — yellow accent for hover state
        {0.00f, 0.00f, 0.00f, 1.00f}  // AiPinStripBg — pure black (matches WindowBg)
    };
    SetAiColors(ai);
}

// Norton Commander 5.51 palette — refined against the canonical 5.51 screenshot
// (en.wikipedia.org/wiki/File:Norton_Commander_5.51.png). The original DOS palette uses TEAL
// (#008080) for the two file panels and dialog chrome, BLUE (#0000AA) for the highlighted row /
// selection, WHITE (#FFFFFF) for panel frames, YELLOW (#FFFF55) for filenames + hotkey accents,
// and BRIGHT CYAN (#55FFFF) for numeric metadata. Dialogs render on light-gray (#C0C0C0) with
// dark text in the real Norton — but ImGui has a single global Text color, so we keep PopupBg
// gray and accept yellow-on-gray for dialogs as the trade-off for the iconic yellow filename look.
// Norton Commander chrome is sharp 1px boxes — no rounded corners anywhere. Split out of
// ApplyNortonCommander for function-size compliance.
void ApplyNortonRounding(ImGuiStyle& style) {
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
}

// Norton Commander C++ syntax palette + AI-bubble palette. Split out of ApplyNortonCommander for
// function-size compliance; the color values are unchanged.
void ApplyNortonSyntaxAndAi() {
    // C++ syntax palette — NC panel tones (BuildSyntaxColorsForTheme).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::NortonCommander));
    // AI palette — Norton Commander 5.51 DOS aesthetic. Bubble uses NC bright yellow
    // (the iconic filename accent) tinted at 0.22 alpha against the teal panel bg.
    // Role labels use yellow (user) / bright cyan (assistant) per the NC palette
    // convention — yellow accents go on user actions, cyan on system / output.
    const SmatchetThemeAiColors ai = {
        {1.00f, 1.00f, 0.333f, 0.22f}, // AiUserBubbleBg — NC yellow, 0.22 alpha
        {1.00f, 1.00f, 0.333f, 1.00f}, // AiUserRoleLabel — NC bright yellow
        {0.333f, 1.00f, 1.00f, 1.00f}, // AiAssistantRoleLabel — NC bright cyan
        {0.80f, 0.80f, 0.80f, 1.00f},  // AiActionRowIcon — NC light grey
        {1.00f, 1.00f, 0.333f, 1.00f}, // AiActionRowIconHover — NC bright yellow accent
        {0.00f, 0.00f, 0.667f, 0.95f}  // AiPinStripBg — NC selection blue (#0000AA-ish)
    };
    SetAiColors(ai);
}

void ApplyNortonCommander(ImGuiStyle& style, ImVec4* colors) {
    // Inverted Norton palette: blue dominates panels/chrome, teal becomes the accent / selection.
    const ImVec4 ncTeal = ImVec4(0.00f, 0.00f, 0.667f, 1.00f);      // #0000AA — panel bg (was teal)
    const ImVec4 ncTealBright = ImVec4(0.00f, 0.00f, 0.85f, 1.00f); // #0000DD — panel header strip (brighter blue)
    const ImVec4 ncBlue = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);       // #008080 — selection bar (was blue, now teal)
    const ImVec4 ncBrightCyan = ImVec4(0.333f, 1.00f, 1.00f, 1.0f); // #55FFFF — accent
    const ImVec4 ncCyan = ImVec4(0.00f, 1.00f, 1.00f, 1.00f);       // #00FFFF — body text + borders + scrollbars
    const ImVec4 ncWhite = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);      // #FFFFFF
    const ImVec4 ncYellow = ImVec4(1.00f, 1.00f, 0.333f, 1.00f);    // #FFFF55 — highlight text
    const ImVec4 ncGray = ImVec4(0.667f, 0.667f, 0.667f, 1.00f);    // #AAAAAA — NC 2.01 menu bar bg

    // Norton Commander's chrome is sharp 1px boxes — no rounded corners on any region.
    ApplyNortonRounding(style);

    // Body text = yellow (filename color). Most iconic NC tone — reads on teal panels, blue
    // selection bars, and (less ideally) on gray popups.
    colors[ImGuiCol_Text] = ncCyan;
    colors[ImGuiCol_TextDisabled] = ImVec4(0.667f, 0.667f, 0.667f, 1.00f); // #AAAAAA
    colors[ImGuiCol_WindowBg] = ncTeal;
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ncBlue; // dropdown menu = teal in inverted palette (matches NC 2.01 menu dropdown)
    colors[ImGuiCol_Border] = ncCyan;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.30f, 1.00f);

    // Input fields sit on the accent (teal in this inverted palette).
    colors[ImGuiCol_FrameBg] = ncBlue;
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.00f, 0.667f, 0.667f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ncBrightCyan;

    // Title bars use the brighter blue header strip.
    colors[ImGuiCol_TitleBg] = ncTealBright;
    colors[ImGuiCol_TitleBgActive] = ncBlue;
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.30f, 0.80f);

    colors[ImGuiCol_MenuBarBg] = ncGray; // NC 2.01 top menu bar — gray strip
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ncCyan;
    colors[ImGuiCol_ScrollbarGrabHovered] = ncBrightCyan;
    colors[ImGuiCol_ScrollbarGrabActive] = ncYellow;

    colors[ImGuiCol_CheckMark] = ncWhite;
    colors[ImGuiCol_SliderGrab] = ncWhite;
    colors[ImGuiCol_SliderGrabActive] = ncBrightCyan;

    // Buttons mimic the dialog Ok/Cancel — gray surface with yellow text. Active inverts to blue.
    colors[ImGuiCol_Button] = ncGray;
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ncBlue;

    // Selected row = teal bg with yellow text (inverted Norton highlight).
    colors[ImGuiCol_Header] = ncBlue;
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.667f, 0.667f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ncBrightCyan;

    colors[ImGuiCol_Separator] = ncCyan;
    colors[ImGuiCol_SeparatorHovered] = ncBrightCyan;
    colors[ImGuiCol_SeparatorActive] = ncYellow;

    colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ncWhite;

    colors[ImGuiCol_Tab] = ncTeal;
    colors[ImGuiCol_TabHovered] = ncBrightCyan;
    colors[ImGuiCol_TabActive] = ncBlue;
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.30f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ncTeal;

    colors[ImGuiCol_DockingPreview] = ImVec4(0.00f, 0.667f, 0.667f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.00f, 0.00f, 0.30f, 1.00f);

    colors[ImGuiCol_PlotLines] = ncBrightCyan;
    colors[ImGuiCol_PlotLinesHovered] = ncYellow;
    colors[ImGuiCol_PlotHistogram] = ncYellow;
    colors[ImGuiCol_PlotHistogramHovered] = ncWhite;

    // Table header = brighter blue strip. Alt-rows on a faint teal tint.
    colors[ImGuiCol_TableHeaderBg] = ncTealBright;
    colors[ImGuiCol_TableBorderStrong] = ncCyan;
    colors[ImGuiCol_TableBorderLight] = ImVec4(1.00f, 1.00f, 1.00f, 0.40f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.40f, 0.40f, 0.40f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.667f, 0.667f, 0.55f);
    colors[ImGuiCol_DragDropTarget] = ncYellow;
    colors[ImGuiCol_NavHighlight] = ncYellow;
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 0.333f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.20f, 0.40f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.20f, 0.55f);

    ApplyNortonSyntaxAndAi();
}

// Pristine ImGui-built-in dark palette — bright cyan-blue accents (HeaderHovered #4296FA,
// WindowBg #0F0F0F). Default for fresh installs. Deliberately bypasses ApplyCommonStyle so
// rounding / padding stay at ImGui defaults (the whole point of this theme is to show what
// ImGui ships with out of the box).
void ApplyImGuiDefaultDark(ImGuiStyle& style, ImVec4* /*colors*/) {
    ImGui::StyleColorsDark(&style);

    // Muted, neutral syntax palette readable on ImGui-default-dark bg (#0F0F0F). Mirrors
    // SmatchetDark's family (BuildSyntaxColorsForTheme treats ImGuiDefaultDark identically).
    SetSyntaxColors(SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId::ImGuiDefaultDark));
    // AI palette — ImGui default-dark. Accent is #4296FA (the ImGui HeaderHovered)
    // = (0.26, 0.59, 0.98). Bubble at 0.18 alpha reads cleanly against the
    // 0.06-luma WindowBg (#0F0F0F) without fighting the bright accent on hover.
    const SmatchetThemeAiColors ai = {
        {0.26f, 0.59f, 0.98f, 0.18f}, // AiUserBubbleBg — #4296FA, 0.18 alpha
        {0.36f, 0.69f, 1.00f, 1.00f}, // AiUserRoleLabel — accent at full
        {0.85f, 0.75f, 1.00f, 1.00f}, // AiAssistantRoleLabel — purple
        {0.50f, 0.50f, 0.50f, 1.00f}, // AiActionRowIcon — TextDisabled equivalent
        {0.92f, 0.92f, 0.92f, 1.00f}, // AiActionRowIconHover — Text
        {0.10f, 0.10f, 0.12f, 0.90f}  // AiPinStripBg — slightly deeper than WindowBg
    };
    SetAiColors(ai);
}

} // namespace

SmatchetThemeSyntaxColors SmatchetTheme::BuildSyntaxColorsForTheme(ThemeId theme) {
    switch (theme) {
    case ThemeId::Vs2022Dark:
        // VS Code Dark+ canonical: keyword #569CD6, string #CE9178, comment #6A9955,
        // number #B5CEA8, preproc #9B9B9B, identifier #9CDCFE.
        return {{0.34f, 0.61f, 0.84f, 1.0f}, {0.81f, 0.57f, 0.47f, 1.0f}, {0.42f, 0.60f, 0.33f, 1.0f},
                {0.71f, 0.81f, 0.66f, 1.0f}, {0.61f, 0.61f, 0.61f, 1.0f}, {0.61f, 0.86f, 0.99f, 1.0f}};
    case ThemeId::Vs2022Light:
        // VS Code Light+ canonical (legible on white): keyword #0000FF, string #A31515,
        // comment #008000, number #098658, preproc #808080, identifier #001080.
        return {{0.00f, 0.00f, 1.00f, 1.0f}, {0.64f, 0.08f, 0.08f, 1.0f}, {0.00f, 0.50f, 0.00f, 1.0f},
                {0.04f, 0.53f, 0.35f, 1.0f}, {0.50f, 0.50f, 0.50f, 1.0f}, {0.00f, 0.06f, 0.50f, 1.0f}};
    case ThemeId::HighContrast:
        // Fully saturated primaries for low-vision / accessibility audits on pure black.
        return {{1.00f, 1.00f, 0.00f, 1.0f}, {1.00f, 0.00f, 1.00f, 1.0f}, {0.00f, 1.00f, 0.00f, 1.0f},
                {0.00f, 1.00f, 1.00f, 1.0f}, {1.00f, 0.65f, 0.00f, 1.0f}, {1.00f, 1.00f, 1.00f, 1.0f}};
    case ThemeId::NortonCommander:
        // NC panel tones — keyword bright yellow (#FFFF55, B channel 0.333 to stay
        // pairwise-distinct from HighContrast's pure yellow), string light red, comment
        // light gray, number bright cyan, preproc bright green, identifier bright white.
        return {{1.00f, 1.00f, 0.333f, 1.0f}, {1.00f, 0.50f, 0.50f, 1.0f},   {0.667f, 0.667f, 0.667f, 1.0f},
                {0.333f, 1.00f, 1.00f, 1.0f}, {0.333f, 1.00f, 0.333f, 1.0f}, {1.00f, 1.00f, 1.00f, 1.0f}};
    case ThemeId::ModernDark:
    case ThemeId::ImGuiDefaultDark:
    case ThemeId::SmatchetDark:
    default:
        // Legacy SmatchetDark family (purple/orange/green/cyan/yellow); ModernDark +
        // ImGuiDefaultDark share it (only chrome diverges). Identifier = soft sky-blue.
        return {{0.78f, 0.50f, 1.00f, 1.0f}, {0.95f, 0.65f, 0.45f, 1.0f}, {0.45f, 0.75f, 0.45f, 1.0f},
                {0.65f, 0.85f, 1.00f, 1.0f}, {0.85f, 0.85f, 0.50f, 1.0f}, {0.62f, 0.80f, 0.92f, 1.0f}};
    }
}

void SmatchetTheme::ApplyStyle(ThemeId theme) {
    ImGuiStyle& style = ImGui::GetStyle();

    // Full-style reset BEFORE the theme override runs. Two layered concerns:
    //   1) Colors[]: ApplySmatchetDark / ApplyModernDark / ... only write 55 of the ~66
    //      ImGuiCol_* slots. The unwritten slots (TextLink, TreeLines, InputTextCursor,
    //      TabSelectedOverline, TabDimmedSelectedOverline, DragDropTargetBg, UnsavedMarker,
    //      NavCursor + renamed-alias duplicates) keep whatever value the previous theme — or a
    //      leaked PushStyleColor — left in them.
    //   2) Layout fields (WindowBorderSize, ChildBorderSize, IndentSpacing, TabBarBorderSize,
    //      etc.): NortonCommander zeroes the 7 rounding fields; ApplyCommonStyle resets those
    //      symmetrically. But the ~50 OTHER ImGuiStyle layout fields (DisabledAlpha,
    //      WindowMinSize, ItemInnerSpacing, CellPadding, TabBarBorderSize, ScrollbarPadding,
    //      DragDropTargetPadding, AntiAliased* toggles, HoverDelay*, etc.) are not touched by
    //      ApplyCommonStyle. Any future per-theme override on one of those fields would leak
    //      across switches — the exact failure mode the user reports as "switching back doesn't
    //      restore SmatchetDark even after the seed-colors fix."
    // `style = ImGuiStyle{}` invokes ImGui's default ctor — every layout field returns to its
    // ImGui-documented baseline, and the freshly-constructed Colors[] holds the dark seed
    // (per the ctor body in imgui.cpp). We then re-run StyleColorsLight() for the lone light
    // variant; StyleColorsDark for everything else (the dark seed is already in the ctor, but
    // calling it explicitly is the documented public API and keeps the intent obvious).
    // ApplyCommonStyle + per-theme apply then layer on top of a known-clean substrate.
    style = ImGuiStyle{};
    if (theme == ThemeId::Vs2022Light) {
        ImGui::StyleColorsLight(&style);
    } else {
        ImGui::StyleColorsDark(&style);
    }

    // Semantic status-text palette (P2-H9) — set for EVERY theme, including the
    // ImGuiDefaultDark early-return path below.
    SetSemanticColors(SmatchetTheme::BuildSemanticColorsForTheme(theme));

    // ImGuiDefaultDark short-circuits the Smatchet brand polish: this theme exists to surface
    // ImGui's pristine defaults (rounding, padding, HeaderHovered #4296FA, WindowBg #0F0F0F),
    // so we skip ApplyCommonStyle entirely. The freshly-constructed style above already holds
    // the ImGui defaults; ApplyImGuiDefaultDark just sets the syntax palette and (re)asserts
    // StyleColorsDark for clarity. Returning early keeps a future ApplyCommonStyle bug from
    // silently overwriting the defaults.
    if (theme == ThemeId::ImGuiDefaultDark) {
        ApplyImGuiDefaultDark(style, style.Colors);
        ReapplyHostDensityScale(style);
        return;
    }

    ApplyCommonStyle(style);

    // `colors` is a convenience alias for the (now-reset, seeded) Colors[] array. The per-theme
    // ApplyXxx helpers take it as their `ImVec4* colors` parameter so they only have to write the
    // slot indices, not re-derefence the style on every line.
    ImVec4* colors = style.Colors;

    switch (theme) {
    case ThemeId::ModernDark:
        ApplyModernDark(style, colors);
        break;
    case ThemeId::Vs2022Dark:
        ApplyVs2022Dark(style, colors);
        break;
    case ThemeId::Vs2022Light:
        ApplyVs2022Light(style, colors);
        break;
    case ThemeId::HighContrast:
        ApplyHighContrast(style, colors);
        break;
    case ThemeId::NortonCommander:
        ApplyNortonCommander(style, colors);
        break;
    case ThemeId::ImGuiDefaultDark:
        // Handled by the early-return above; this case is here so the switch is exhaustive
        // and a compiler warning fires if the enum gains a new value without an arm.
        break;
    case ThemeId::SmatchetDark:
    default:
        ApplySmatchetDark(style, colors);
        break;
    }

    // Re-assert the host density scale on the freshly rebuilt style. Mobile seam
    // number 13, a no-op on desktop where the scale stays 1.0. Must run before the
    // revision bump so consumers that snapshot the counter see the fully scaled style.
    ReapplyHostDensityScale(style);

    // Bump the theme-revision counter so consumers (CodeColorView's tokenize
    // cache, future per-theme caches) know to invalidate. Increment AFTER the
    // per-theme write completes so a parallel reader doesn't see a half-baked
    // palette tagged with the new revision. Slice 3 of
    // code-syntax-coloring-and-tooltips.
    g_themeRevision.fetch_add(1, std::memory_order_release);
}

void SmatchetTheme::ApplyUiDensityScale(float densityScale) {
    // Guard non-positive / NaN — `!(x > 0)` rejects NaN too. Leave the current style untouched.
    if (!(densityScale > 0.0f)) {
        return;
    }
    // Store so every later ApplyStyle (which rebuilds the style from baseline) can re-assert it.
    g_hostDensityScale = densityScale;
    ImGui::GetStyle().ScaleAllSizes(densityScale);
}

float SmatchetTheme::HostDensityScale() { return g_hostDensityScale; }

void SmatchetTheme::ReassertHostDensityScale(float newScale) {
    // Guard non-positive / NaN — `!(x > 0)` rejects NaN too. Leave the current style untouched.
    if (!(newScale > 0.0f)) {
        return;
    }
    const float old = g_hostDensityScale;
    // Update the stored scale FIRST so any later ApplyStyle (theme switch) rebuilds the style from
    // baseline at the new scale via ReapplyHostDensityScale.
    g_hostDensityScale = newScale;
    // Transform the LIVE style by only the relative factor. ScaleAllSizes is multiplicative, and the
    // current style already carries `old`, so multiplying by newScale/old lands it on newScale
    // without re-stacking `old`. ShouldRescaleHostDensity gates this to a meaningful, well-defined
    // move (skip first call / no movement / non-positive old) — pure + unit-tested.
    if (ShouldRescaleHostDensity(old, newScale)) {
        ImGui::GetStyle().ScaleAllSizes(newScale / old);
    }
}

void SmatchetTheme::ApplyTouchScale(float scale) {
    // Pure live-style multiply layered on top of the host density base — deliberately does NOT
    // touch g_hostDensityScale (that global is host-owned; the Auto logical-width divisor + mobile
    // band heights read it). Net live scale after a preceding ApplyStyle is host*scale. The == 1.0
    // / non-positive cases are inert (ShouldApplyDensityScale), so a Desktop flip-back is a plain
    // ApplyStyle rebuild that lands back on the untouched host base.
    if (ShouldApplyDensityScale(scale)) {
        ImGui::GetStyle().ScaleAllSizes(scale);
    }
}

const SmatchetThemeSyntaxColors& SmatchetTheme::GetSyntaxColors() { return gSyntaxColors; }

const SmatchetThemeAiColors& SmatchetTheme::GetActiveAiColors() { return gAiColors; }

SmatchetThemeSemanticColors SmatchetTheme::BuildSemanticColorsForTheme(ThemeId theme) {
    switch (theme) {
    case ThemeId::Vs2022Light:
        // Darker, saturated variants — the dark-family pastels drop below 2:1 contrast
        // on the light theme's near-white surfaces (the P2-H9 failure).
        return {ImVec4(0.72f, 0.05f, 0.09f, 1.0f), ImVec4(0.58f, 0.38f, 0.00f, 1.0f),
                ImVec4(0.05f, 0.45f, 0.15f, 1.0f)};
    case ThemeId::HighContrast:
        // Saturated primaries on pure black — matches the syntax-palette philosophy.
        return {ImVec4(1.00f, 0.25f, 0.25f, 1.0f), ImVec4(1.00f, 0.80f, 0.00f, 1.0f),
                ImVec4(0.00f, 1.00f, 0.20f, 1.0f)};
    case ThemeId::NortonCommander:
        // NC panel tones: light red / bright yellow / bright green read on the blue panel.
        return {ImVec4(1.00f, 0.50f, 0.50f, 1.0f), ImVec4(1.00f, 1.00f, 0.33f, 1.0f),
                ImVec4(0.33f, 1.00f, 0.33f, 1.0f)};
    case ThemeId::SmatchetDark:
    case ThemeId::ModernDark:
    case ThemeId::Vs2022Dark:
    case ThemeId::ImGuiDefaultDark:
    default:
        // Dark family — the pre-P2-H9 literals, now routed through the seam.
        return {ImVec4(0.95f, 0.35f, 0.35f, 1.0f), ImVec4(1.00f, 0.85f, 0.30f, 1.0f),
                ImVec4(0.45f, 0.95f, 0.55f, 1.0f)};
    }
}

const SmatchetThemeSemanticColors& SmatchetTheme::GetActiveSemanticColors() { return gSemanticColors; }

std::uint64_t SmatchetTheme::GetThemeRevision() { return g_themeRevision.load(std::memory_order_acquire); }
