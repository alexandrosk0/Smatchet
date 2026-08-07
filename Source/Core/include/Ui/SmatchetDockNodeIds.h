#pragma once

#include "imgui.h"
#include <cstddef>
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

// Returns `slot` when that dock node currently exists, else 0.
//
// Guard every SetNextWindowDockID(<a constant above>, ImGuiCond_Always) with this. The
// constants name slots the DEFAULT layout cuts, and any of them can be gone at runtime:
// the user dragged the last tab out and the node collapsed, the .ini came from an older
// schema that never had it, or (kSecondarySideBar) no DockBuilder call ever cut it at all.
// Docking to a dead id does not fail loudly — ImGui mints an orphan root node at the
// window's current rect, so the window LOOKS docked while owning no slot in the dockspace,
// and the illusion only breaks when the user drags a splitter. Skipping the write instead
// leaves the window floating: visibly wrong, therefore fixable.
ImGuiID EnsureDockSlotAlive(ImGuiID slot);

// The passive half of the pattern above: dock the next window into `slot` on FIRST USE only.
// No-op when the slot is dead (EnsureDockSlotAlive) or while a mouse button is held, since a
// dock write mid-drag fights the drag the user is performing.
void DockNextWindowOnFirstUse(ImGuiID slot);

// Iteration over every registered layout key (windows that have a default dock slot).
// Used by the live layout reset to arm a force-redock of all default windows.
size_t DefaultDockLayoutKeyCount();
const char* DefaultDockLayoutKeyAt(size_t i);
} // namespace SmatchetDockNodeIds
