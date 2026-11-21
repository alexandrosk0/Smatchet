#pragma once

// The one tooltip that names the view-switch keys (view.next / view.previous), shared by the views sidebar
// search and the grid header's view selector. The bound keys are read from the live bindings, so a rebind
// shows at once. The text rules live in ViewPickerPure.h (doctested); this owns the localized labels and the
// draw. Plain ::ImGui: include above a TU's `#define ImGui SmatchetLocalizedImGui` line or below it.

#include "Config/KeybindingsConfig.h"
#include "SmatchetLocalization.h"
#include "ViewPickerPure.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace SmatchetViewSwitchTooltip {

/// Shows `lead` (already localized) followed by the bound view-switch keys as the current item's tooltip.
/// Call after the item, while it is hovered.
inline void Show(const char* lead, const std::vector<Keybinding>& bindings) {
    const std::string tip = smatchet::views::ViewSwitchKeysTooltip(
        lead != nullptr ? lead : "", bindings, SmatchetLocalization::T("views.next_view_key", "Next view:"),
        SmatchetLocalization::T("views.previous_view_key", "Previous view:"));
    ::ImGui::SetTooltip("%s", tip.c_str());
}

} // namespace SmatchetViewSwitchTooltip
