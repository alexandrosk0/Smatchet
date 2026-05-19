#pragma once

#include "ConfigManager.h"
#include "imgui.h"

namespace smatchet {
namespace ui_density {

/** Push the user's chosen density into the active ImGuiStyle.
 *
 * SmatchetTheme::ApplyStyle rewrites ItemSpacing + FramePadding to Normal-density
 * defaults via its ApplyCommonStyle pass. Theme switches and density changes both
 * route through this helper so a theme cycle (Compact → switch theme → still Compact)
 * never silently reverts the padding to Normal. */
inline void ApplyDensityToImGuiStyle(TrackerConfig::UiDensity density) {
    ImGuiStyle& style = ::ImGui::GetStyle();
    switch (density) {
    case TrackerConfig::UiDensity::Compact:
        style.ItemSpacing = ImVec2(4.0f, 2.0f);
        style.FramePadding = ImVec2(4.0f, 2.0f);
        break;
    case TrackerConfig::UiDensity::Comfortable:
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 6.0f);
        break;
    default:
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.FramePadding = ImVec2(6.0f, 4.0f);
        break;
    }
}

} // namespace ui_density
} // namespace smatchet
