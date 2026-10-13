#pragma once

#include "imgui.h"

namespace smatchet {
namespace ui {

/// Opens the modal popup `popupId` and sizes the next window to `widthFrac` x `heightFrac` of the main
/// viewport, centered. Call at the same window depth as the matching BeginPopupModal, outside any
/// table, so ImGui anchors the popup to a stable top-level window (cells only flag "just opened").
inline void OpenCenteredModalPopup(const char* popupId, float widthFrac, float heightFrac) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::OpenPopup(popupId);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * widthFrac, viewport->Size.y * heightFrac), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
}

} // namespace ui
} // namespace smatchet
