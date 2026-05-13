#pragma once

#include "imgui.h"
#include <cstdint>

// Dockspace node IDs assigned by the default layout ini.
// Primary side bar = split-left node, bottom panel = split-down node.
// Secondary side bar is reserved; the node id won't exist until the user creates it.
namespace SmatchetDockNodeIds {
constexpr ImGuiID kPrimarySideBar = 0x00000004u;
constexpr ImGuiID kBottomPanel = 0x0000000Au;
constexpr ImGuiID kSecondarySideBar = 0x00000010u;
} // namespace SmatchetDockNodeIds
