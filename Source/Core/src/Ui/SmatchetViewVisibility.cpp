#include "SmatchetViewVisibility.h"

#include "ConfigManager.h"

// Panel visibility is driven by the cfg.Show* flags that gate each ImGui::Begin call.
// ImGui collapses empty dock nodes automatically — no HiddenTabBar manipulation needed.
// HiddenTabBar only hides the tab strip visually; it does not collapse the node and
// fights the layout engine on window resize.

void SetViewVisible(TrackerConfig& cfg, ViewSlot slot, bool visible) {
    switch (slot) {
    case ViewSlot::PrimarySideBar:
        cfg.ShowPrimarySideBar = visible;
        break;
    case ViewSlot::SecondarySideBar:
        cfg.ShowSecondarySideBar = visible;
        break;
    case ViewSlot::BottomPanel:
        cfg.ShowPanel = visible;
        break;
    }
}
