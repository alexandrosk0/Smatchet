#include "SmatchetViewVisibility.h"

#include "ConfigManager.h"
#include "Logger.h"

#include "imgui.h"
#include "imgui_internal.h"

// Dockspace node IDs assigned by the default layout ini.
// Primary side bar = split-left node, bottom panel = split-down node.
// Secondary side bar is reserved; the node id won't exist until the user creates it.
static const ImGuiID kNodePrimarySideBar   = 0x00000004u;
static const ImGuiID kNodeBottomPanel      = 0x0000000Au;
static const ImGuiID kNodeSecondarySideBar = 0x00000010u;

static void ApplyNodeVisibility(ImGuiID nodeId, bool visible) {
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeId);
    if (!node) {
        LOG_DEBUG("SmatchetViewVisibility: node 0x%08X not found (layout not yet built)", nodeId);
        return;
    }
    if (visible) {
        // Clear the hidden-tab-bar flag so the node and its tabs appear again.
        node->LocalFlags &= ~ImGuiDockNodeFlags_HiddenTabBar;
        node->WantHiddenTabBarToggle = false;
    } else {
        node->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
    }
}

void SetViewVisible(TrackerConfig& cfg, bool& panelFlag, ViewSlot slot, bool visible) {
    panelFlag = visible;

    ImGuiID nodeId = 0;
    switch (slot) {
        case ViewSlot::PrimarySideBar:
            nodeId = kNodePrimarySideBar;
            cfg.ShowPrimarySideBar = visible;
            break;
        case ViewSlot::SecondarySideBar:
            nodeId = kNodeSecondarySideBar;
            cfg.ShowSecondarySideBar = visible;
            break;
        case ViewSlot::BottomPanel:
            nodeId = kNodeBottomPanel;
            cfg.ShowPanel = visible;
            break;
    }

    ApplyNodeVisibility(nodeId, visible);
}
