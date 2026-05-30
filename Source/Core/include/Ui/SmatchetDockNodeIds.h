#pragma once

#include "imgui.h"
#include <cstdint>

// Dockspace node IDs assigned by the default layout ini.
// Every content window maps to exactly one of these slots; overlays/popups are excluded.
namespace SmatchetDockNodeIds {
constexpr ImGuiID kCentralNode = 0x00000002u;
constexpr ImGuiID kViewsColumn = 0x00000008u;
constexpr ImGuiID kPrimarySideBar = 0x00000004u;
constexpr ImGuiID kBottomPanel = 0x0000000Au;
constexpr ImGuiID kSecondarySideBar = 0x00000010u;

// Returns the default dock slot for a layout key, or 0 if the window is an overlay/popup.
ImGuiID DefaultDockSlotForLayoutKey(const char* layoutKey);
} // namespace SmatchetDockNodeIds
