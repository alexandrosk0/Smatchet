#include "SmatchetViewVisibility.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"

#include "imgui.h"
#include "imgui_internal.h"

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

void SetViewVisible(TrackerConfig& cfg, ViewSlot slot, bool visible) {
    ImGuiID nodeId = 0;
    switch (slot) {
    case ViewSlot::PrimarySideBar:
        nodeId = SmatchetDockNodeIds::kPrimarySideBar;
        cfg.ShowPrimarySideBar = visible;
        break;
    case ViewSlot::SecondarySideBar:
        nodeId = SmatchetDockNodeIds::kSecondarySideBar;
        cfg.ShowSecondarySideBar = visible;
        break;
    case ViewSlot::BottomPanel:
        nodeId = SmatchetDockNodeIds::kBottomPanel;
        cfg.ShowPanel = visible;
        break;
    }

    ApplyNodeVisibility(nodeId, visible);
}
