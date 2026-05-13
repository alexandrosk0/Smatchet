#pragma once

#include "SmatchetThemeIds.h"
#include "imgui.h"

namespace SmatchetTheme {
    /** Apply the named style palette to the current ImGui context. */
    void ApplyStyle(ThemeId theme);

    /** Predefined colors for status and priorities. */
    namespace Colors {
        const ImVec4 StatusDone = ImVec4(0.25f, 0.60f, 0.30f, 1.0f);     // Green
        const ImVec4 StatusInProgress = ImVec4(0.15f, 0.45f, 0.85f, 1.0f); // Blue
        const ImVec4 StatusToDo = ImVec4(0.40f, 0.40f, 0.45f, 1.0f);     // Grey/Muted
        const ImVec4 StatusBlocked = ImVec4(0.80f, 0.20f, 0.20f, 1.0f);  // Red
        
        const ImVec4 PriorityHigh = ImVec4(0.90f, 0.30f, 0.30f, 1.0f);   // Red
        const ImVec4 PriorityMedium = ImVec4(0.90f, 0.60f, 0.20f, 1.0f); // Orange
        const ImVec4 PriorityLow = ImVec4(0.30f, 0.70f, 0.40f, 1.0f);    // Green
    }
}






