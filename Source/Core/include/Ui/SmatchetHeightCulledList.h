#pragma once

#include "imgui.h"

#include <cstddef>
#include <vector>

namespace smatchet {
namespace ui {

/// Draws `heights.size()` stacked items top to bottom inside a scrolling region. An item whose last
/// drawn height is known (> item spacing) and is off-screen draws as a spacer instead of re-emitting
/// its content every frame (Pillar 1: long comment threads / History blobs). `heights[i]` is the last
/// drawn height of item i (incl. its trailing item spacing), <= 0 when not yet measured; the caller
/// resets it when the width or font size changes. `drawItem(i)` draws item i under `PushID(i)`.
template <typename DrawItem> void DrawHeightCulledItems(std::vector<float>& heights, float width, DrawItem&& drawItem) {
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        float& height = heights[i];
        if (height > itemSpacingY && !ImGui::IsRectVisible(ImVec2(width, height))) {
            ImGui::Dummy(ImVec2(width, height - itemSpacingY)); // Dummy re-adds the trailing spacing
            continue;
        }
        const float startY = ImGui::GetCursorPosY();
        ImGui::PushID(static_cast<int>(i));
        drawItem(i);
        ImGui::PopID();
        height = ImGui::GetCursorPosY() - startY;
    }
}

} // namespace ui
} // namespace smatchet
