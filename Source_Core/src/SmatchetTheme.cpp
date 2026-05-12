#include "SmatchetTheme.h"

namespace {

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
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.14f, 0.14f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);

    colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);

    colors[ImGuiCol_CheckMark]              = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.45f, 0.65f, 1.00f, 1.00f);

    colors[ImGuiCol_Button]                 = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);

    colors[ImGuiCol_Header]                 = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);

    colors[ImGuiCol_Separator]              = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.35f, 0.55f, 0.95f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);

    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.35f, 0.55f, 0.95f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.35f, 0.55f, 0.95f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.35f, 0.55f, 0.95f, 0.95f);

    colors[ImGuiCol_Tab]                    = ImVec4(0.18f, 0.18f, 0.22f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.35f, 0.55f, 0.95f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.25f, 0.35f, 0.55f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.12f, 0.12f, 0.14f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);

    colors[ImGuiCol_DockingPreview]         = ImVec4(0.35f, 0.55f, 0.95f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.35f, 0.55f, 0.95f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.35f, 0.55f, 0.95f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

// Flatter, more neutral dark — slightly cooler greys and a desaturated blue accent.
void ApplyModernDark(ImGuiStyle& /*style*/, ImVec4* colors) {
    colors[ImGuiCol_Text]                   = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.48f, 0.50f, 0.54f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);

    colors[ImGuiCol_TitleBg]                = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.45f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.40f, 0.42f, 0.46f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);

    colors[ImGuiCol_CheckMark]              = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);

    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);

    colors[ImGuiCol_Separator]              = ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.45f, 0.65f, 0.95f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);

    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.45f, 0.65f, 0.95f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.45f, 0.65f, 0.95f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.45f, 0.65f, 0.95f, 0.95f);

    colors[ImGuiCol_Tab]                    = ImVec4(0.16f, 0.17f, 0.20f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.45f, 0.65f, 0.95f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.28f, 0.38f, 0.55f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.10f, 0.11f, 0.13f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);

    colors[ImGuiCol_DockingPreview]         = ImVec4(0.45f, 0.65f, 0.95f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);

    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.22f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.45f, 0.65f, 0.95f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.45f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

// Visual Studio 2022 "Dark" palette — near-black editor bg (#1E1E1E), darker chrome (#2D2D30),
// VS Code Dark+ blue accent (#007ACC). Familiar to Visual Studio / VS Code users.
void ApplyVs2022Dark(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 accent       = ImVec4(0.00f, 0.48f, 0.80f, 1.00f); // #007ACC
    const ImVec4 accentBright = ImVec4(0.10f, 0.58f, 0.90f, 1.00f);

    colors[ImGuiCol_Text]                   = ImVec4(0.94f, 0.94f, 0.94f, 1.00f); // #F1F1F1
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.12f, 0.12f, 1.00f); // #1E1E1E
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.18f, 0.18f, 0.19f, 0.98f); // #2D2D30
    colors[ImGuiCol_Border]                 = ImVec4(0.27f, 0.27f, 0.28f, 0.60f); // #434346
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.20f, 0.21f, 1.00f); // #333337
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.27f, 0.27f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.32f, 0.32f, 0.34f, 1.00f);

    colors[ImGuiCol_TitleBg]                = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.18f, 0.18f, 0.19f, 1.00f); // #2D2D30
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.12f, 0.12f, 0.12f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.34f, 0.34f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.44f, 0.44f, 0.46f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.54f, 0.54f, 0.56f, 1.00f);

    colors[ImGuiCol_CheckMark]              = accent;
    colors[ImGuiCol_SliderGrab]             = accent;
    colors[ImGuiCol_SliderGrabActive]       = accentBright;

    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.27f, 0.40f, 0.56f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = accent;

    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.27f, 0.40f, 0.56f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = accent;

    colors[ImGuiCol_Separator]              = ImVec4(0.27f, 0.27f, 0.28f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.00f, 0.48f, 0.80f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = accent;

    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.00f, 0.48f, 0.80f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.00f, 0.48f, 0.80f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.00f, 0.48f, 0.80f, 0.95f);

    colors[ImGuiCol_Tab]                    = ImVec4(0.18f, 0.18f, 0.19f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.00f, 0.48f, 0.80f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.07f, 0.30f, 0.52f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.12f, 0.12f, 0.12f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);

    colors[ImGuiCol_DockingPreview]         = ImVec4(0.00f, 0.48f, 0.80f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);

    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.32f, 0.32f, 0.34f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.24f, 0.24f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.00f, 0.48f, 0.80f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = accent;
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

// Visual Studio 2022 "Light" / VS Code Light+ palette — white editor bg, #EEEEF2 chrome,
// VS blue accent (#007ACC) carried over from the dark theme for consistency.
void ApplyVs2022Light(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 accent       = ImVec4(0.00f, 0.48f, 0.80f, 1.00f); // #007ACC
    const ImVec4 accentBright = ImVec4(0.10f, 0.58f, 0.90f, 1.00f);

    colors[ImGuiCol_Text]                   = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.97f, 0.97f, 0.98f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.70f, 0.70f, 0.74f, 0.60f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.93f, 0.93f, 0.95f, 1.00f); // #EEEEF2
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);

    colors[ImGuiCol_TitleBg]                = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(1.00f, 1.00f, 1.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.97f, 0.97f, 0.98f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.74f, 0.74f, 0.76f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.60f, 0.60f, 0.64f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.48f, 0.48f, 0.52f, 1.00f);

    colors[ImGuiCol_CheckMark]              = accent;
    colors[ImGuiCol_SliderGrab]             = accent;
    colors[ImGuiCol_SliderGrabActive]       = accentBright;

    colors[ImGuiCol_Button]                 = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = accent;

    colors[ImGuiCol_Header]                 = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.80f, 0.88f, 0.96f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = accent;

    colors[ImGuiCol_Separator]              = ImVec4(0.70f, 0.70f, 0.74f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.00f, 0.48f, 0.80f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = accent;

    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.00f, 0.48f, 0.80f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.00f, 0.48f, 0.80f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.00f, 0.48f, 0.80f, 0.95f);

    colors[ImGuiCol_Tab]                    = ImVec4(0.88f, 0.88f, 0.92f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.00f, 0.48f, 0.80f, 0.40f);
    colors[ImGuiCol_TabActive]              = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.93f, 0.93f, 0.95f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);

    colors[ImGuiCol_DockingPreview]         = ImVec4(0.00f, 0.48f, 0.80f, 0.45f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);

    colors[ImGuiCol_PlotLines]              = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.45f, 0.00f, 1.00f);

    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.88f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.62f, 0.62f, 0.65f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);

    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.00f, 0.48f, 0.80f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.90f, 0.60f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = accent;
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(0.30f, 0.30f, 0.30f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

// High contrast — pure black background, pure white text, cyan accent. Maximum legibility for
// low-vision users / accessibility audits.
void ApplyHighContrast(ImGuiStyle& /*style*/, ImVec4* colors) {
    const ImVec4 cyan = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);

    colors[ImGuiCol_Text]                   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_Border]                 = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]                = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.00f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);

    colors[ImGuiCol_TitleBg]                = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.00f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = cyan;
    colors[ImGuiCol_ScrollbarGrabActive]    = cyan;

    colors[ImGuiCol_CheckMark]              = cyan;
    colors[ImGuiCol_SliderGrab]             = cyan;
    colors[ImGuiCol_SliderGrabActive]       = cyan;

    colors[ImGuiCol_Button]                 = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = cyan;

    colors[ImGuiCol_Header]                 = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = cyan;

    colors[ImGuiCol_Separator]              = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_SeparatorHovered]       = cyan;
    colors[ImGuiCol_SeparatorActive]        = cyan;

    colors[ImGuiCol_ResizeGrip]             = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered]      = cyan;
    colors[ImGuiCol_ResizeGripActive]       = cyan;

    colors[ImGuiCol_Tab]                    = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TabHovered]             = cyan;
    colors[ImGuiCol_TabActive]              = ImVec4(0.00f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.00f, 0.00f, 0.00f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);

    colors[ImGuiCol_DockingPreview]         = ImVec4(0.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]         = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    colors[ImGuiCol_PlotLines]              = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = cyan;
    colors[ImGuiCol_PlotHistogram]          = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = cyan;

    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(1.00f, 1.00f, 1.00f, 0.80f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(1.00f, 1.00f, 1.00f, 0.40f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);

    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.00f, 1.00f, 1.00f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = cyan;
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.70f);
}

} // namespace

void SmatchetTheme::ApplyStyle(ThemeId theme) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    ApplyCommonStyle(style);

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
        case ThemeId::SmatchetDark:
        default:
            ApplySmatchetDark(style, colors);
            break;
    }
}
