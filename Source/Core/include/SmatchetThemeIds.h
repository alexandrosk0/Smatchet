#pragma once
#include <cstdint>

// Identifier for the active ImGui style palette. Kept in a lightweight header
// (no ImGui dependency) so TrackerConfig and other config-layer code can carry
// the value without pulling the full ImGui surface.
enum class ThemeId : std::uint8_t {
    SmatchetDark = 0, // Muted-navy palette — bit-identical to the legacy hard-coded SmatchetDark.
    ModernDark = 1,
    Vs2022Dark = 2,
    Vs2022Light = 3,
    HighContrast = 4,
    NortonCommander = 5,
    // Bright cyan-blue ImGui-built-in dark palette (HeaderHovered #4296FA, WindowBg #0F0F0F).
    // Default for fresh installs.
    ImGuiDefaultDark = 6
};
