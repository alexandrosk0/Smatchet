#pragma once

// P4V-style drag gesture for the bottom dock panel (SmatchetDockNodeIds::kBottomPanel)
// and the right-hand side columns (kViewsColumn, kPrimarySideBar, kSecondarySideBar):
// dragging the splitter before the panel past its minimum size hides it - closing EVERY
// registered tab docked in it (Views, Assistant, bottom windows dragged in; grid panes
// stay and keep the panel visible) - and a slim grab strip along the far edge (the bottom
// of the work area / the right edge of the column's parent split) reveals the remembered
// tab set again by dragging back. The side columns are driven by Tick plus the
// CollapseSideBar / ExpandSideBar entry points (View > Appearance and the view.sidebar.*
// commands); Collapse and Expand are the programmatic entry points for the bottom panel,
// behind the View > Panel menu toggle and the view.panel.bottom command, so every surface
// shares one remember-the-open-tabs + remember-the-height mechanism.

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

// View > Appearance side bars. Primary is the whole right side (the Views column 0x08 AND
// the primary side bar 0x04, whatever tabs they hold); Secondary is the secondary side bar
// 0x10. Visible = the live dock node shows content. Collapse hides every registered tab in
// each visible column (remembering the set) and keeps cfg.ShowPrimarySideBar /
// ShowSecondarySideBar in step with what remains visible. Expand reopens the remembered
// sets and returns whether anything was opened: Primary falls back to the Views dashboard
// when nothing was remembered; Secondary is a no-op (returns false) in that case.
enum class SideBar { Primary, Secondary };

bool IsSideBarVisible(SideBar bar);
void CollapseSideBar(UiDrawSession& d, SideBar bar);
bool ExpandSideBar(UiDrawSession& d, SideBar bar);

// Whether ExpandSideBar would open something (remembered tabs, or the Views fallback when
// the Views dashboard is not already open elsewhere). The menu disables a hidden item when not.
bool CanExpandSideBar(const UiDrawSession& d, SideBar bar);

// Keyboard resize (view.panel.grow / view.panel.shrink, the keyboard equivalent of dragging the splitter
// before a panel). Focused picks the edge panel whose dock node holds the focused window; Primary is the
// first visible of the Views column and the primary side bar.
enum class PanelTarget { Focused, Bottom, Primary, Secondary };

struct PanelNudgeResult {
    bool Ok = false;
    const char* Panel = "";      // "bottom" / "primary" / "secondary" once resolved
    float ExtentPx = 0.0f;       // the extent asked for, after the same clamp a drag gets
    const char* Error = "";      // why nothing changed, when !Ok
    bool NoFocusedPanel = false; // Focused target only: the focused window is in no panel, so nothing to resize
};

// Changes the visible panel's extent (height for the bottom panel, width for a side bar) by deltaPx.
// The new extent is applied by Tick over the next frames, through the same deferred SizeRef write a
// reveal drag uses, so it is clamped and saved with the layout like a drag. Fails, changing nothing,
// when no panel resolves, the panel is hidden, or the layout has no splitter before it.
PanelNudgeResult NudgePanelExtent(UiDrawSession& d, PanelTarget target, float deltaPx);

// One keyboard step: two lines of the current font, so it scales with the UI zoom.
float KeyboardNudgeStepPx();

} // namespace SmatchetBottomPanelDrag
