#pragma once

namespace smatchet {
namespace ui {

/// Gives an open, vertically scrollable tooltip first claim on mouse-wheel input
/// before ImGui's NewFrame can scroll the window underneath it.
void RouteWheelToScrollableTooltipBeforeNewFrame();

/// True while any active tooltip hierarchy owns a vertical scrollbar.
bool HasOpenScrollableTooltip();

} // namespace ui
} // namespace smatchet
