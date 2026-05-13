#pragma once
#include <cstdint>

// Identifier for the active ImGui style palette. Kept in a lightweight header
// (no ImGui dependency) so TrackerConfig and other config-layer code can carry
// the value without pulling the full ImGui surface.
enum class ThemeId : std::uint8_t {
    SmatchetDark = 0, // Default — bit-identical to the legacy hard-coded palette.
    ModernDark = 1,
    Vs2022Dark = 2,
    Vs2022Light = 3,
    HighContrast = 4
};
