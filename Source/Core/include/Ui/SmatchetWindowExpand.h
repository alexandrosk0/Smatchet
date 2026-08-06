#pragma once

// Per-window "Expand to fullscreen" toggle (docs/plans/shipped/window-expand-button.md).
// Every dockable secondary window and every non-main grid pane gets a control
// immediately LEFT of its close X: it pins that window over the main viewport
// WORK AREA (below the menu bar, above the status bar — both stay reachable),
// covering every other view; the same control then minimizes it back.
//
// Expanding stores the window's dock id (plus its rect when floating), undocks it
// ONCE via DockContextProcessUndockWindow(clear_persistent_docking_ref=false), then
// re-issues SetNextWindowDockID(0) + SetNextWindowPos/Size(WorkPos/WorkSize) each
// frame before Begin; minimizing replays the stored dock id for one frame. Keeping
// the persistent dock ref is what makes the node SURVIVE an expand of its last tab:
// DockNodeRemoveWindow only destroys an emptied leaf when the departing window
// cleared that ref, and letting SetNextWindowDockID(0) do the undock clears it.
//
// Four limits this deliberately does NOT solve — none is invisible, and none
// should be assumed away by later work:
//  - It is NOT .ini-safe. The override IS the window's live state, and ImGui's
//    settings writer snapshots Pos/Size/DockId off the live window (dropping the
//    DockId line entirely while DockId == 0). So the debounced auto-save, and the
//    unconditional save in DestroyContext, persist an expanded window as floating
//    and fullscreen. On relaunch SmatchetUI_Layout's pendingReDockWindows path
//    force-redocks it within ~2 frames — to its DEFAULT slot, so a customised dock
//    placement is lost. Fixing that needs an ini-safe transition, not a comment.
//  - The node-survival trick above covers OUR undock, not a node killed by anything
//    else while expanded (a sibling being dragged out, an .ini reload). If the saved
//    id resolves to nothing, the restore cuts the slot again from the recorded split
//    (side + ratio): same PLACE, but a NEW id, because DockBuilderSplitNode returns
//    an auto-generated child id. Any other window that replays the old canonical
//    constant then docks into nothing. Only when the rebuild also finds nothing
//    alive does it fall back to the pre-expand float rect, after which
//    repairTopLevelWindow re-docks to the default slot.
//  - Keeping the leaf alive does NOT cost a visible empty pane, which is worth
//    stating because it looks like it should. An emptied non-central leaf goes
//    IsVisible = false (DockNodeUpdateVisibleFlag), and DockNodeTreeUpdatePosSize
//    skips its whole sizing block unless BOTH children are visible — so the sibling
//    is handed the full parent rect and SizeRef is never rewritten. The pane
//    silently yields its space and minimize restores the original ratio. (A CENTRAL
//    node is the exception: it stays visible while empty, by design.) The leaf lives
//    until the next settings load, where DockContextPruneUnusedSettingsNodes takes it.
//  - On a FULL tab bar the button overlaps the last tab and takes its clicks.
//    DockNodeCalcTabBarLayout reserves room for the node's own close X, not for
//    this control, so once the tabs fill BarRect the two occupy the same pixels.
//    A real fix has to shrink BarRect (or claim a section) rather than draw over it.
//
// The state below stays ImGui-free
// (ImGuiID is a plain `unsigned int`) so the transition core in
// SmatchetWindowExpand_detail.cpp links into bucket-A without a UI loop.

#include <unordered_map>
#include <vector>

struct UiDrawSession;

namespace SmatchetWindowExpand {

/// Where a window sat before it was expanded. `DockId == 0` means it was floating,
/// in which case Pos/Size carry its rect (a docked window's rect belongs to its node).
/// The Split* fields describe the SLOT rather than the node, for the last-resort case
/// where something OTHER than our own undock destroyed the leaf while we were
/// expanded, leaving `DockId` naming nothing. They carry enough to cut the same slot
/// again — which side of which split, at which ratio — with the sibling recorded
/// because the parent is the node that may have been merged over.
struct WindowExpandSaved {
    unsigned int DockId = 0;
    float PosX = 0.0f;
    float PosY = 0.0f;
    float SizeX = 0.0f;
    float SizeY = 0.0f;
    unsigned int SplitParentId = 0;  ///< Node that split to make DockId; 0 when it had no parent.
    unsigned int SplitSiblingId = 0; ///< The other child of that split.
    int SplitDir = -1;               ///< ImGuiDir of DockId within the split; -1 = unknown.
    float SplitRatio = 0.5f;         ///< DockId's share of the parent along the split axis.
};

/// A docked window's toggle, deferred to end-of-frame. It is drawn by re-Begin-ing
/// the dock HOST window, so it is only issued once every window has ended rather
/// than nested inside our own Begin.
struct WindowExpandTabBarButton {
    unsigned int NodeId = 0;   ///< Dock node over whose tab bar the button is drawn.
    unsigned int WindowId = 0; ///< Window the button toggles.
    WindowExpandSaved Current; ///< Placement captured while the window was current.
};

/// Session-lived expand state. At most one window is expanded at a time.
struct WindowExpandState {
    /// ImGuiID of the fullscreen window; 0 when nothing is expanded.
    unsigned int ExpandedId = 0;
    /// Consume-once focus for the transition frame — re-focusing every frame would trap focus.
    bool FocusOnExpand = false;
    /// Pre-expand placement by window ImGuiID; an entry lives until its restore is
    /// consumed, so expanding B while A is expanded still returns A to A's own slot.
    std::unordered_map<unsigned int, WindowExpandSaved> Saved;
    /// Ids awaiting the one-frame dock + geometry replay that un-expands them.
    std::vector<unsigned int> RestorePending;
    /// Ids that submitted this frame — drives the end-of-frame self-heal.
    std::vector<unsigned int> SubmittedIds;
    /// Docked toggles collected this frame; drained (and cleared) by EndFrame.
    std::vector<WindowExpandTabBarButton> PendingTabBarButtons;
};

/// Call immediately BEFORE ImGui::Begin. Records the submission and applies either
/// the fullscreen geometry or a pending restore. `windowName` must be Begin's string.
void BeginWindow(UiDrawSession& d, const char* windowName);

/// Call immediately AFTER a Begin that returned true. Draws the Expand / Minimize
/// control immediately left of the close X — over the dock node's tab bar when the
/// window is docked, in its title bar when it is floating — and applies a click.
void DrawToggle(UiDrawSession& d);

/// True when `windowName` is the expanded window. Callable BEFORE its Begin (unlike
/// IsCurrentWindowExpanded), so a window can drop ImGuiWindowFlags_NoTitleBar for the
/// expanded frames — without a title bar there is nowhere to draw the minimize half.
bool IsWindowExpanded(const UiDrawSession& d, const char* windowName);

/// True when the window being submitted is the expanded one. SmatchetUI::repairTopLevelWindow
/// checks this: an expanded window is deliberately undocked, and force-redocking it would
/// undo the expansion on the next frame.
bool IsCurrentWindowExpanded(const UiDrawSession& d);

/// Toggle by name rather than by click — used by bucket-E (no command is registered for it
/// today), since the docked button lives in the dock host's amended tab bar and has no item
/// path a test can drive.
void ToggleWindow(UiDrawSession& d, const char* windowName);

/// Drop any expansion and clear every latch (layout reset, where the forced redock
/// would fight the fullscreen pin).
void Reset(UiDrawSession& d);

/// Once per frame, after every window has submitted. Runs the self-heal.
void EndFrame(UiDrawSession& d);

// Pure transition core (SmatchetWindowExpand_detail.cpp), mirroring SmatchetGridHeaderUi_detail.
namespace detail {

/// Apply one toggle of `id`: expanded -> minimize (arm a restore), else expand (arming a
/// restore for whatever was expanded before). `current` is stored only on the expand branch.
void ApplyToggle(WindowExpandState& s, unsigned int id, const WindowExpandSaved& current);

/// Consume a pending restore for `id`, writing `out` and dropping the entry. False when none armed.
bool ConsumeRestore(WindowExpandState& s, unsigned int id, WindowExpandSaved& out);

/// End-of-frame self-heal: an expanded window that did not submit is gone, so drop the
/// expansion and arm its restore (it re-docks when next reopened). Always clears SubmittedIds.
void SelfHeal(WindowExpandState& s);

} // namespace detail

} // namespace SmatchetWindowExpand
