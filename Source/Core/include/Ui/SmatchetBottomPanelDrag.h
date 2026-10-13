#pragma once

// P4V-style drag gesture for the bottom dock panel (SmatchetDockNodeIds::kBottomPanel)
// and the right-hand side columns (kViewsColumn, kPrimarySideBar, kSecondarySideBar):
// dragging the splitter before the panel past its minimum size hides it - closing EVERY
// registered tab docked in it (Views, Assistant, bottom windows dragged in; grid panes
// stay and keep the panel visible) - and a slim grab strip along the far edge (the bottom
// of the work area / the right edge of the column's parent split) reveals the remembered
// tab set again by dragging back. The side columns are driven only by Tick; Collapse and
// Expand below are the programmatic entry points for the bottom panel, behind the
// View > Panel menu toggle and the view.panel.bottom command, so every surface shares
// one remember-the-open-tabs + remember-the-height mechanism.

struct UiDrawSession;

namespace SmatchetBottomPanelDrag {

// Per-frame gesture driver for all panels. Call once per desktop frame after the docked
// windows have submitted (the dock tree for this frame is settled by then). Owns: the
// splitter drag-to-hide detection, the release-to-hide overlay hint, the reveal grip,
// and the deferred panel-size application after a reveal.
void Tick(UiDrawSession& d);

// Live visibility of the bottom dock node: the node exists inside the dockspace and has
// visible content. This - not the persisted cfg.ShowPanel flag - is the truth the
// menu checkmark and the toggle command key off, because the user can also empty the
// panel by closing each tab individually.
bool IsPanelVisible();

// Hide the panel: remember which bottom-docked windows are open (and the panel height),
// clear their show flags so the emptied node collapses, and flip cfg.ShowPanel off.
void Collapse(UiDrawSession& d);

// Reveal the panel: reopen the remembered tabs (the Log window when nothing is
// remembered, e.g. a fresh session) and re-apply the panel height. desiredHeightPx <= 0
// means "remembered height, else the default"; a positive value (the live reveal drag)
// wins over both. Flips cfg.ShowPanel on.
void Expand(UiDrawSession& d, float desiredHeightPx);

} // namespace SmatchetBottomPanelDrag
