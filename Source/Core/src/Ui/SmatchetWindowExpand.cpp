// ImGui half of the per-window expand toggle. The pure state machine lives in
// SmatchetWindowExpand_detail.cpp; this file only captures placement, draws the
// control and pins the expanded window over the viewport work area.

#include "SmatchetWindowExpand.h"

#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace SmatchetWindowExpand {

namespace {

// "##" hides the label text while keeping the id RELATIVE to the id stack — a
// "###" label restarts the hash, which would give every window's toggle the same
// ImGuiID. The glyph is drawn from primitives, not typed, so the control shows the
// standard maximize / restore mark even where no icon font is installed.
const char* const kToggleLabel = "##SmatchetWindowExpandToggle";

/// The title-bar maximize / restore-down marks, drawn like ImGui draws its own
/// close X — from primitives, so they never depend on a glyph being in the atlas.
void DrawToggleGlyph(ImDrawList* dl, const ImVec2& rectMin, const ImVec2& rectMax, bool expanded) {
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    const float sz = ImMin(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
    const ImVec2 c((rectMin.x + rectMax.x) * 0.5f, (rectMin.y + rectMax.y) * 0.5f);
    const float th = ImMax(1.0f, IM_ROUND(sz * 0.09f));
    if (!expanded) {
        const float h = IM_ROUND(sz * 0.32f);
        dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 0.0f, 0, th);
        return;
    }
    // Restore-down: the front square, plus only the still-visible outline of the
    // one behind it (drawn as a polyline whose ends die on the front square).
    const float h = IM_ROUND(sz * 0.28f);
    const float off = IM_ROUND(ImMax(2.0f, sz * 0.14f));
    const ImVec2 fMin(c.x - h - off, c.y - h + off);
    const ImVec2 fMax(c.x + h - off, c.y + h + off);
    const ImVec2 bMin(c.x - h + off, c.y - h - off);
    const ImVec2 bMax(c.x + h + off, c.y + h - off);
    dl->AddRect(fMin, fMax, col, 0.0f, 0, th);
    ImVec2 back[5];
    back[0] = ImVec2(bMin.x, fMin.y);
    back[1] = ImVec2(bMin.x, bMin.y);
    back[2] = ImVec2(bMax.x, bMin.y);
    back[3] = ImVec2(bMax.x, bMax.y);
    back[4] = ImVec2(fMax.x, bMax.y);
    dl->AddPolyline(back, 5, col, ImDrawFlags_None, th);
}

const char* ToggleTooltip(bool expanded) {
    if (expanded) {
        return SmatchetLocalization::T("window.minimize.tooltip", "Minimize (restore previous position)");
    }
    return SmatchetLocalization::T("window.expand.tooltip", "Expand over the whole workspace");
}

/// Placement to replay on minimize. DockNode (not DockId) is the live-docked test:
/// DockId lingers on a window that was undocked, and replaying it would re-dock a
/// window the user had deliberately floated.
WindowExpandSaved CapturePlacement(const ImGuiWindow* window) {
    WindowExpandSaved out;
    out.DockId = (window->DockNode != NULL) ? static_cast<unsigned int>(window->DockNode->ID) : 0u;
    out.PosX = window->Pos.x;
    out.PosY = window->Pos.y;
    out.SizeX = window->SizeFull.x;
    out.SizeY = window->SizeFull.y;

    // Describe the SLOT as well as the node — see WindowExpandSaved: the node id
    // alone does not survive undocking the last tab out of it.
    const ImGuiDockNode* node = window->DockNode;
    const ImGuiDockNode* parent = (node != NULL) ? node->ParentNode : NULL;
    if (parent == NULL) {
        return out;
    }
    const bool firstChild = (parent->ChildNodes[0] == node);
    const ImGuiDockNode* sibling = parent->ChildNodes[firstChild ? 1 : 0];
    out.SplitParentId = static_cast<unsigned int>(parent->ID);
    out.SplitSiblingId = (sibling != NULL) ? static_cast<unsigned int>(sibling->ID) : 0u;
    const bool horizontal = (parent->SplitAxis == ImGuiAxis_X);
    out.SplitDir =
        horizontal ? (firstChild ? ImGuiDir_Left : ImGuiDir_Right) : (firstChild ? ImGuiDir_Up : ImGuiDir_Down);
    const float parentSize = horizontal ? parent->Size.x : parent->Size.y;
    const float ownSize = horizontal ? node->Size.x : node->Size.y;
    // Clamped: a degenerate ratio would rebuild the slot as an invisible sliver.
    out.SplitRatio = (parentSize > 1.0f) ? ImClamp(ownSize / parentSize, 0.05f, 0.95f) : 0.5f;
    return out;
}

/// Cuts `restore`'s slot again after its node was merged away, and docks
/// `windowName` into it. Returns the new node id, or 0 when neither end of the
/// recorded split is still around to cut.
unsigned int RebuildDockSlot(const char* windowName, const WindowExpandSaved& restore) {
    if (restore.SplitDir < 0) {
        return 0u;
    }
    // Prefer the parent, which is what the sibling was merged INTO and so the usual
    // survivor, then fall back to the sibling for a tree that shifted the other way.
    ImGuiDockNode* target = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(restore.SplitParentId));
    if (target == NULL) {
        target = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(restore.SplitSiblingId));
    }
    if (target == NULL) {
        return 0u;
    }
    const ImGuiID root = ImGui::DockNodeGetRootNode(target)->ID;
    // DockBuilder does its tree surgery IMMEDIATELY, and this runs mid-frame from a
    // pre-Begin path, so it can re-dock windows that already ran their Begin this
    // frame. Tolerated only because this is the last-resort fallback: the normal
    // restore replays a still-live node id and never reaches here.
    ImGuiID atDir = 0;
    ImGui::DockBuilderSplitNode(target->ID, static_cast<ImGuiDir>(restore.SplitDir), restore.SplitRatio, &atDir, NULL);
    if (atDir == 0) {
        return 0u;
    }
    ImGui::DockBuilderDockWindow(windowName, atDir);
    ImGui::DockBuilderFinish(root);
    return static_cast<unsigned int>(atDir);
}

/// The control itself. Both paths place it by absolute screen pos inside a window
/// whose layout cursor belongs to someone else, so the DC is saved and restored and
/// the item is submitted on the menu nav layer, exactly as ImGui's own title-bar
/// buttons do. Returns true on click.
bool DrawToggleButtonAt(ImGuiWindow* window, const ImVec2& pos, float buttonSz, const ImRect& clip, bool expanded) {
    const ImVec2 cursorBackup = window->DC.CursorPos;
    const ImVec2 cursorMaxBackup = window->DC.CursorMaxPos;
    const ImGuiNavLayer navBackup = window->DC.NavLayerCurrent;
    window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;
    ImGui::PushClipRect(clip.Min, clip.Max, false);
    ImGui::SetCursorScreenPos(pos);
    // Transparent idle fill so it reads as a title-bar affordance, not a widget.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool clicked = ImGui::Button(kToggleLabel, ImVec2(buttonSz, buttonSz));
    // Hover-gated: SetItemTooltip would run the localization lookup every frame for
    // every window, and this control is submitted once per open window per frame.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", ToggleTooltip(expanded));
    }
    DrawToggleGlyph(window->DrawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), expanded);
    ImGui::PopStyleColor();
    ImGui::PopClipRect();
    window->DC.CursorPos = cursorBackup;
    window->DC.CursorMaxPos = cursorMaxBackup;
    window->DC.NavLayerCurrent = navBackup;
    return clicked;
}

/// Floating path: a manual title-bar button laid out exactly like ImGui's own
/// close/collapse buttons (RenderWindowTitleBarContents), one slot further left.
void DrawTitleBarToggle(WindowExpandState& s, ImGuiWindow* window, unsigned int id, bool expanded,
                        const WindowExpandSaved& current) {
    if ((window->Flags & ImGuiWindowFlags_NoTitleBar) != 0) {
        return;
    }
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImRect titleBar = window->TitleBarRect();
    const float buttonSz = g.FontSize;
    float padR = style.FramePadding.x;
    if (window->HasCloseButton) {
        padR += buttonSz + style.ItemInnerSpacing.x;
    }
    const bool hasCollapse =
        (window->Flags & ImGuiWindowFlags_NoCollapse) == 0 && style.WindowMenuButtonPosition != ImGuiDir_None;
    if (hasCollapse && style.WindowMenuButtonPosition == ImGuiDir_Right) {
        padR += buttonSz + style.ItemInnerSpacing.x;
    }
    const ImVec2 pos(titleBar.Max.x - padR - buttonSz, titleBar.Min.y + style.FramePadding.y);
    if (pos.x <= titleBar.Min.x) {
        return;
    }

    if (DrawToggleButtonAt(window, pos, buttonSz, titleBar, expanded)) {
        detail::ApplyToggle(s, id, current);
    }
}

} // namespace

void BeginWindow(UiDrawSession& d, const char* windowName) {
    if (windowName == NULL || windowName[0] == '\0') {
        return;
    }
    WindowExpandState& s = d.windowExpand;
    const unsigned int id = static_cast<unsigned int>(ImHashStr(windowName));
    s.SubmittedIds.push_back(id);

    WindowExpandSaved restore;
    if (detail::ConsumeRestore(s, id, restore)) {
        // Our own undock keeps the node alive (see the expand branch below), so the
        // saved id normally still resolves and is replayed as-is. It can still be GONE
        // if something else killed the node while we were expanded — a sibling dragged
        // out, an .ini reload. Replaying a dead id does not fail loudly: ImGui mints an
        // orphan root at the window's current rect, i.e. a fullscreen-sized float that
        // merely LOOKS docked. So a dead id falls through to RebuildDockSlot, and only
        // then to the pre-expand float rect.
        unsigned int dockTo =
            (restore.DockId != 0 && ImGui::DockBuilderGetNode(static_cast<ImGuiID>(restore.DockId)) != NULL)
                ? restore.DockId
                : 0u;
        if (dockTo == 0 && restore.DockId != 0) {
            // Cut the slot again rather than dropping the window to a float that merely
            // LOOKS docked — such a float survives until the user drags a dock splitter,
            // at which point the panes resize out from under it and it visibly pops out.
            dockTo = RebuildDockSlot(windowName, restore);
        }
        ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockTo), ImGuiCond_Always);
        if (dockTo == 0) {
            ImGui::SetNextWindowPos(ImVec2(restore.PosX, restore.PosY), ImGuiCond_Always);
            if (restore.SizeX > 0.0f && restore.SizeY > 0.0f) {
                ImGui::SetNextWindowSize(ImVec2(restore.SizeX, restore.SizeY), ImGuiCond_Always);
            }
        }
        return;
    }
    if (s.ExpandedId != id) {
        return;
    }
    // Undock ONCE, explicitly, KEEPING the window's persistent dock ref. ImGui only
    // destroys an emptied leaf when the departing window cleared that ref — see
    // DockNodeRemoveWindow's `window->DockId != node->ID` guard. Letting
    // SetNextWindowDockID(0) perform the undock clears it, so expanding the LAST tab
    // out of a node kills the node. Its id is a canonical layout constant that other
    // windows replay verbatim, and docking into a dead id does not fail loudly:
    // DockContextBindNodeToWindow mints an orphan root inheriting the window's current
    // rect — a float that merely LOOKS docked until a splitter drag resizes the real
    // panes out from under it.
    ImGuiWindow* expanded = ImGui::FindWindowByName(windowName);
    if (expanded != NULL && expanded->DockNode != NULL) {
        ImGui::DockContextProcessUndockWindow(ImGui::GetCurrentContext(), expanded,
                                              /*clear_persistent_docking_ref=*/false);
    }
    // Re-pinned every frame: a docked sibling being dragged, or an .ini reload,
    // would otherwise pull the fullscreen window back into the tree.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    if (s.FocusOnExpand) {
        ImGui::SetNextWindowFocus();
        s.FocusOnExpand = false;
    }
}

void DrawToggle(UiDrawSession& d) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    // SkipItems is what a Begin returning false leaves behind (collapsed, clipped, or an
    // unselected dock tab). Bailing here is what keeps the docked path to exactly one
    // queued button per node even at the call sites that ignore Begin's return value.
    if (window == NULL || window->SkipItems) {
        return;
    }
    WindowExpandState& s = d.windowExpand;
    const unsigned int id = static_cast<unsigned int>(window->ID);
    const WindowExpandSaved current = CapturePlacement(window);

    if (window->DockNode != NULL && window->DockNode->TabBar != NULL) {
        // Docked: the X lives on the node's tab bar, so the toggle has to go
        // there too. Only the selected tab's Begin returns true, so each node
        // contributes exactly one entry per frame.
        WindowExpandTabBarButton pending;
        pending.NodeId = static_cast<unsigned int>(window->DockNode->ID);
        pending.WindowId = id;
        pending.Current = current;
        s.PendingTabBarButtons.push_back(pending);
        return;
    }
    DrawTitleBarToggle(s, window, id, s.ExpandedId == id, current);
}

bool IsCurrentWindowExpanded(const UiDrawSession& d) {
    if (d.windowExpand.ExpandedId == 0) {
        return false;
    }
    const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    return window != NULL && d.windowExpand.ExpandedId == static_cast<unsigned int>(window->ID);
}

bool IsWindowExpanded(const UiDrawSession& d, const char* windowName) {
    if (windowName == NULL || windowName[0] == '\0' || d.windowExpand.ExpandedId == 0) {
        return false;
    }
    // ImHashStr, not FindWindowByName: this is callable BEFORE the window's Begin,
    // and a window that has never submitted has no ImGuiWindow to look up.
    return d.windowExpand.ExpandedId == static_cast<unsigned int>(ImHashStr(windowName));
}

void ToggleWindow(UiDrawSession& d, const char* windowName) {
    if (windowName == NULL || windowName[0] == '\0') {
        return;
    }
    ImGuiWindow* window = ImGui::FindWindowByName(windowName);
    // Liveness, not just non-NULL: FindWindowByName keeps returning a window that has
    // ever existed, with stale Pos/DockNode. Toggling one of those would store a bogus
    // home rect and strand a RestorePending latch that force-replays it whenever the
    // window is next opened. Both flags are needed — `Active` alone misses a call made
    // before the window's Begin this frame, and `WasActive` alone rejects a window
    // that appeared on THIS frame and has never had a previous one to be active in.
    if (window == NULL || !(window->Active || window->WasActive)) {
        LOG_WARN("WindowExpand: no live window named '%s' to toggle", windowName);
        return;
    }
    detail::ApplyToggle(d.windowExpand, static_cast<unsigned int>(window->ID), CapturePlacement(window));
}

void Reset(UiDrawSession& d) {
    WindowExpandState& s = d.windowExpand;
    s.ExpandedId = 0;
    s.FocusOnExpand = false;
    s.Saved.clear();
    s.RestorePending.clear();
    s.SubmittedIds.clear();
    s.PendingTabBarButtons.clear();
}

void EndFrame(UiDrawSession& d) {
    WindowExpandState& s = d.windowExpand;
    // Drained here, outside every Begin/End pair: this re-Begins the dock HOST
    // window, which must not nest inside a docked child.
    for (size_t i = 0; i < s.PendingTabBarButtons.size(); ++i) {
        // By value: ApplyToggle runs before the last read of `pending` below, and a
        // reference into the vector would be one refactor away from dangling.
        const WindowExpandTabBarButton pending = s.PendingTabBarButtons[i];
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(pending.NodeId));
        if (node == NULL || node->TabBar == NULL || node->HostWindow == NULL) {
            continue;
        }
        // NOT a trailing TabItemButton: TabBarLayout right-aligns the trailing section
        // only when the tabs already overflow (`tab_offset = ImMin(BarRect.W - section.W,
        // tab_offset)`), so with room to spare it packs left, against the last tab. Drawn
        // manually instead, off BarRect.Max.x — DockNodeCalcTabBarLayout has already
        // shrunk that edge by the node's close X, so this lands exactly one slot left of it.
        const ImRect bar = node->TabBar->BarRect;
        const float buttonSz = ImGui::GetFontSize();
        const ImVec2 pos(bar.Max.x - buttonSz, bar.Min.y + ImGui::GetStyle().FramePadding.y);
        if (pos.x <= bar.Min.x) {
            continue;
        }
        ImGuiWindow* host = node->HostWindow;
        bool clicked = false;
        if (ImGui::Begin(host->Name)) {
            // Seeded with the node id: the "##" label keeps the id relative, so this is
            // what keeps one node's toggle distinct from every other window's.
            ImGui::PushOverrideID(static_cast<ImGuiID>(pending.NodeId));
            // A docked window is never the expanded one (expanding undocks it), so the
            // tab-bar control is always the "expand" face.
            clicked = DrawToggleButtonAt(host, pos, buttonSz, bar, false);
            ImGui::PopID();
        }
        ImGui::End();
        if (clicked) {
            detail::ApplyToggle(s, pending.WindowId, pending.Current);
        }
    }
    s.PendingTabBarButtons.clear();

    // Collapsing an expanded window is a dead end otherwise: Begin returns false, so
    // DrawToggle never draws the minimize face, and BeginWindow already recorded the
    // submission so SelfHeal does not rescue it either — a fullscreen-width title bar
    // with no way back. Treat the collapse as the minimize the user could not click.
    if (s.ExpandedId != 0) {
        ImGuiWindow* expanded = ImGui::FindWindowByID(static_cast<ImGuiID>(s.ExpandedId));
        if (expanded != NULL && expanded->Collapsed) {
            // The expanded branch of ApplyToggle only arms the restore; `current` is unread.
            detail::ApplyToggle(s, s.ExpandedId, WindowExpandSaved());
        }
    }
    detail::SelfHeal(s);
}

} // namespace SmatchetWindowExpand
