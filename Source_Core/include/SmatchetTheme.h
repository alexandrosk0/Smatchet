#pragma once

#include "SmatchetThemeIds.h"
#include "imgui.h"

/** Per-theme palette for the C++ syntax tokenizer in CppSyntaxHighlight. */
struct SmatchetThemeSyntaxColors {
    float Keyword[4];
    float String[4];
    float Comment[4];
    float Number[4];
    float Preprocessor[4];
};

namespace SmatchetTheme {
/** Apply the named style palette to the current ImGui context. */
void ApplyStyle(ThemeId theme);

/** Active theme's C++ syntax-highlight palette. Updated by ApplyStyle. */
const SmatchetThemeSyntaxColors& GetSyntaxColors();

/** Predefined colors for status and priorities. */
namespace Colors {
const ImVec4 StatusDone = ImVec4(0.25f, 0.60f, 0.30f, 1.0f);       // Green
const ImVec4 StatusInProgress = ImVec4(0.15f, 0.45f, 0.85f, 1.0f); // Blue
const ImVec4 StatusToDo = ImVec4(0.40f, 0.40f, 0.45f, 1.0f);       // Grey/Muted
const ImVec4 StatusBlocked = ImVec4(0.80f, 0.20f, 0.20f, 1.0f);    // Red

const ImVec4 PriorityHigh = ImVec4(0.90f, 0.30f, 0.30f, 1.0f);   // Red
const ImVec4 PriorityMedium = ImVec4(0.90f, 0.60f, 0.20f, 1.0f); // Orange
const ImVec4 PriorityLow = ImVec4(0.30f, 0.70f, 0.40f, 1.0f);    // Green
} // namespace Colors
} // namespace SmatchetTheme
