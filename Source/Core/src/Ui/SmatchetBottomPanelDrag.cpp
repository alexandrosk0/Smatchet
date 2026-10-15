#include "SmatchetBottomPanelDrag.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetBottomPanelDragPure.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace pure = SmatchetBottomPanelDragPure;

namespace {

// One row per closable dockable window (mirrors the layout-key table of
// SmatchetDockNodeIds). ONE registry serves every edge panel: collapsing a panel closes
// every registered window docked anywhere in its node subtree, so a window dragged
// between the bottom panel and a side panel hides/restores correctly from either.
// Grid panes ("###GridPane:<id>", "Smatchet - Active Project") are deliberately absent:
// they stay as leftovers and keep the panel visible. sourceTitle is the English source
// title handed to Begin(); FindWindowByName goes through the same WindowTitleFromSource
// transform the localized Begin wrapper applies, so lookups hit in every locale (the
// selectDockedTab pattern).
struct ClosableWindowEntry {
    const char* layoutKey;
    const char* sourceTitle;
    bool UiDrawSession::* showFlag;
    bool UiDrawSession::* focusFlag; // null when the window has no focus latch
};

const ClosableWindowEntry kClosableWindows[] = {
    {"log", "Log", &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus},
    {"preferences", "Preferences", &UiDrawSession::showPreferences, &UiDrawSession::requestPreferencesFocus},
    {"performance", "Performance", &UiDrawSession::showPerformance, &UiDrawSession::requestPerformanceFocus},
    {"audit", "Backend Audit", &UiDrawSession::showAuditTrail, &UiDrawSession::requestAuditTrailFocus},
    {"bulk_import", "Bulk Import Issues", &UiDrawSession::showBulkImport, &UiDrawSession::requestBulkImportFocus},
    {"bulk_export", "Bulk export tickets", &UiDrawSession::showBulkExport, &UiDrawSession::requestBulkExportFocus},
    {"annotate", "Annotate###AnnotateAnalysisModal", &UiDrawSession::showAnnotateAnalysis, nullptr},
    {"plandocs", "Plan Docs", &UiDrawSession::showPlanDocViewer, &UiDrawSession::requestPlanDocViewerFocus},
    {"user_info", "User Info", &UiDrawSession::showUserInfo, &UiDrawSession::requestUserInfoFocus},
    {"notifications", "Notifications", &UiDrawSession::showNotificationCenterWindow,
     &UiDrawSession::requestNotificationCenterFocus},
    // Restore deliberately skips the open latches of these two, so an in-progress draft
    // survives hide/reveal instead of being re-seeded.
    {"quick_create", "Quick Create Issue", &UiDrawSession::showQuickCreateIssue, nullptr},
    {"bug_report", "Report a Bug", &UiDrawSession::showBugReport, nullptr},
    // The ### id hashes the same whatever "Views - <backend>" prefix the Begin wrapper
    // builds, so this one source title finds the window in every state.
    {"views", "Views###SmatchetViewsDashboard", &UiDrawSession::showViewsDashboard,
     &UiDrawSession::requestViewsDashboardFocus},
#if defined(SMATCHET_WITH_AI)
    {"assistant", "Smatchet Assistant", &UiDrawSession::assistantPanelOpen, &UiDrawSession::requestAssistantFocus},
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    {"scripting", "Scripting", &UiDrawSession::showLuaAutomationWindow, &UiDrawSession::requestLuaAutomationFocus},
#endif
#if defined(SMATCHET_WITH_MCP)
    {"mcp", "MCP Server", &UiDrawSession::showMcpServerWindow, &UiDrawSession::requestMcpServerFocus},
#endif
};

// Everything that differs between the panels this driver serves (bottom panel, Views
// column, primary and secondary side bars). All anchor at their far edge (bottom / right),
// which is why the axis-agnostic pure helpers work for all: the "extent" is height for the
// bottom panel and width for the side columns.
struct EdgePanelSpec {
    ImGuiID nodeId;
    ImGuiAxis axis;                // splitter axis: Y for the bottom panel, X for the side columns
    const char* defaultRestoreKey; // bottom panel only: revealed when nothing was remembered
    const char* hideHintKey;
    const char* hideHintFallback;
    const char* revealTipKey;
    const char* revealTipFallback;
    const char* gripWindowName; // bottom panel only (side strips are draw-list overlays)
    const char* logName;
    bool TrackerConfig::* cfgFlag; // persisted visibility flag kept in step (null: none)
    EdgePanelDragState UiDrawSession::* state;
};

const EdgePanelSpec kBottomSpec = {SmatchetDockNodeIds::kBottomPanel,
                                   ImGuiAxis_Y,
                                   "log",
                                   "panel.release_to_hide",
                                   "Release to hide the panel",
                                   "panel.drag_to_open",
                                   "Drag up to open the panel",
                                   "##BottomPanelRevealGrip",
                                   "BottomPanel",
                                   &TrackerConfig::ShowPanel,
                                   &UiDrawSession::bottomPanelDrag};

// The right-hand columns. Whichever of them holds the tabbed Views/Assistant set, the
// gesture hides every registered tab in it; a column that is not the far child of an X
// split (or does not exist) simply has no gesture (SplitterBeforeNodeId / EdgePanelNode).
const EdgePanelSpec kViewsSpec = {SmatchetDockNodeIds::kViewsColumn,
                                  ImGuiAxis_X,
                                  nullptr,
                                  "panel.release_to_hide_side",
                                  "Release to hide the side panel",
                                  "panel.drag_to_open_side",
                                  "Drag left to open the side panel",
                                  nullptr,
                                  "ViewsColumn",
                                  &TrackerConfig::ShowPrimarySideBar,
                                  &UiDrawSession::viewsColumnDrag};

const EdgePanelSpec kPrimarySideSpec = {SmatchetDockNodeIds::kPrimarySideBar,
                                        ImGuiAxis_X,
                                        nullptr,
                                        "panel.release_to_hide_side",
                                        "Release to hide the side panel",
                                        "panel.drag_to_open_side",
                                        "Drag left to open the side panel",
                                        nullptr,
                                        "PrimarySideBar",
                                        &TrackerConfig::ShowPrimarySideBar,
                                        &UiDrawSession::primarySideBarDrag};

const EdgePanelSpec kSecondarySideSpec = {SmatchetDockNodeIds::kSecondarySideBar,
                                          ImGuiAxis_X,
                                          nullptr,
                                          "panel.release_to_hide_side",
                                          "Release to hide the side panel",
                                          "panel.drag_to_open_side",
                                          "Drag left to open the side panel",
                                          nullptr,
                                          "SecondarySideBar",
                                          &TrackerConfig::ShowSecondarySideBar,
                                          &UiDrawSession::secondarySideBarDrag};

const EdgePanelSpec* const kSideSpecs[] = {&kViewsSpec, &kPrimarySideSpec, &kSecondarySideSpec};

bool CollapseEdgePanel(UiDrawSession& d, const EdgePanelSpec& spec);
void ExpandEdgePanel(UiDrawSession& d, const EdgePanelSpec& spec, float desiredExtentPx);

ImGuiDockNode* EdgePanelNode(const EdgePanelSpec& spec) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return nullptr;
    }
    ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(spec.nodeId);
    // Orphan roots reloaded from a stale ini LOOK like the panel but sit outside the
    // dockspace — the same rejection SmatchetDockNodeIds::EnsureDockSlotAlive applies.
    if (node == nullptr || node->ParentNode == nullptr) {
        return nullptr;
    }
    return node;
}

bool NodeShowsContent(const ImGuiDockNode* node) {
    return node->IsVisible && (node->Windows.Size > 0 || node->ChildNodes[0] != nullptr);
}

ImGuiWindow* DockHostWindow(const ImGuiDockNode* node) {
    while (node->ParentNode != nullptr) {
        node = node->ParentNode;
    }
    return node->HostWindow;
}

void GatherDockNodeWindows(const ImGuiDockNode* node, std::vector<const ImGuiWindow*>& out) {
    if (node == nullptr) {
        return;
    }
    for (int i = 0; i < node->Windows.Size; ++i) {
        out.push_back(node->Windows[i]);
    }
    GatherDockNodeWindows(node->ChildNodes[0], out);
    GatherDockNodeWindows(node->ChildNodes[1], out);
}

bool WindowInsideNode(const ImGuiWindow* w, ImGuiID nodeId) {
    for (const ImGuiDockNode* n = w->DockNode; n != nullptr; n = n->ParentNode) {
        if (n->ID == nodeId) {
            return true;
        }
    }
    return false;
}

// The one arrangement the gesture supports: the panel is the far (second) child of a split
// along its axis. Anything else (the user rearranged the layout) disables it.
bool IsFarChildOnAxis(const ImGuiDockNode* node, ImGuiAxis axis) {
    const ImGuiDockNode* parent = node->ParentNode;
    return parent != nullptr && parent->SplitAxis == axis && parent->ChildNodes[1] == node;
}

// Coordinate of the edge the panel is anchored to, along the panel axis: the work-area
// bottom for the bottom panel, the right edge of the Views node's parent split for Views
// (the viewport edge when no side bar is docked beside it, else the seam with the side
// bar). False when that edge is unknown (Views node gone / layout rearranged).
bool FarEdge(const EdgePanelSpec& spec, const ImGuiViewport* vp, const ImGuiDockNode* node, float& out) {
    if (spec.axis == ImGuiAxis_Y) {
        out = vp->WorkPos.y + vp->WorkSize.y;
        return true;
    }
    if (node == nullptr || !IsFarChildOnAxis(node, spec.axis)) {
        return false;
    }
    out = node->ParentNode->Pos.x + node->ParentNode->Size.x;
    return true;
}

// True when a hidden side panel has something to give back, i.e. its grip is a candidate.
bool SideGripCandidate(const EdgePanelSpec& spec, const ImGuiDockNode* node, const UiDrawSession& d) {
    return node != nullptr && !NodeShowsContent(node) && !(d.*(spec.state)).restoreKeys.empty();
}

// Two hidden side panels can park their reveal grips at the same x (e.g. 0x08 and 0x04
// both hidden: 0x01 and 0x09 both end at the viewport edge). Only the most recently
// collapsed draws (LIFO); the other appears once that one is revealed. One strip per x also
// means one owner for a mouse press.
bool SideGripWinsAmongCoincident(const UiDrawSession& d, const EdgePanelSpec& spec, const ImGuiViewport* vp,
                                 float myEdge) {
    const unsigned mySeq = (d.*(spec.state)).collapseSeq;
    for (const EdgePanelSpec* other : kSideSpecs) {
        const ImGuiDockNode* otherNode = EdgePanelNode(*other);
        float otherEdge = 0.0f;
        if (other == &spec || !SideGripCandidate(*other, otherNode, d) || !FarEdge(*other, vp, otherNode, otherEdge)) {
            continue; // itself, or visible / nothing to restore / unusable: cannot overlap
        }
        if (!pure::ShouldDrawRevealGrip(myEdge, mySeq, otherEdge, (d.*(other->state)).collapseSeq,
                                        pure::kRevealGripHeightPx * 0.5f)) {
            return false;
        }
    }
    return true;
}

// Mirrors DockNodeTreeUpdateSplitter (imgui.cpp): the splitter between a split node's
// two children is submitted in the dock host window under PushID((int)parent->ID) +
// GetID("##Splitter"), and the host window's ID stack holds only its own ID at that
// point. Reproducing the id pins "the active drag IS the splitter directly before the
// panel" — no cursor or geometry heuristics, no false positives from other
// splitters or scrollbar drags near the panel edge.
ImGuiID SplitterBeforeNodeId(const EdgePanelSpec& spec, const ImGuiDockNode* node) {
    const ImGuiDockNode* parent = node->ParentNode;
    const ImGuiWindow* host = DockHostWindow(node);
    // Only the "panel is the far child of a split along its axis" arrangement has a
    // splitter whose drag toward the edge means "shrink the panel".
    if (parent == nullptr || host == nullptr || !IsFarChildOnAxis(node, spec.axis)) {
        return 0;
    }
    const int parentIdBits = static_cast<int>(parent->ID); // PushID(int) hashes the raw int bytes
    const ImGuiID seed = ImHashData(&parentIdBits, sizeof(parentIdBits), host->ID);
    return ImHashStr("##Splitter", 0, seed);
}

void DrawReleaseToHideHint(const EdgePanelSpec& spec, const ImGuiDockNode* node) {
    ImDrawList* dl = ::ImGui::GetForegroundDrawList();
    const ImVec2 rectMin = node->Pos;
    const ImVec2 rectMax(node->Pos.x + node->Size.x, node->Pos.y + node->Size.y);
    dl->AddRectFilled(rectMin, rectMax, ::ImGui::GetColorU32(ImGuiCol_DockingPreview, 0.35f));
    const char* hint = SmatchetLocalization::T(spec.hideHintKey, spec.hideHintFallback);
    const ImVec2 textSize = ::ImGui::CalcTextSize(hint);
    float textX = rectMin.x + (node->Size.x - textSize.x) * 0.5f;
    float textY = rectMin.y + (node->Size.y - textSize.y) * 0.5f;
    if (spec.axis == ImGuiAxis_Y && node->Size.y < textSize.y + 8.0f) {
        textY = rectMin.y - textSize.y - 4.0f; // panel squeezed to a sliver — hint above it
    } else if (spec.axis == ImGuiAxis_X && node->Size.x < textSize.x + 8.0f) {
        textX = rectMin.x - textSize.x - 4.0f; // column squeezed to a sliver — hint beside it
    }
    dl->AddText(ImVec2(textX, textY), ::ImGui::GetColorU32(ImGuiCol_Text), hint);
}

void TickHideGesture(UiDrawSession& d, const EdgePanelSpec& spec, ImGuiDockNode* node) {
    ImGuiContext& g = *::ImGui::GetCurrentContext();
    EdgePanelDragState& st = d.*(spec.state);
    const ImGuiID splitterId = SplitterBeforeNodeId(spec, node);
    if (splitterId != 0 && g.ActiveId == splitterId) {
        if (st.splitterDragId == 0 && node->Size[spec.axis] > g.Style.WindowMinSize[spec.axis] * 1.5f) {
            // Drag start: capture the pre-drag extent NOW. By the release that hides
            // the panel the splitter has long clamped the node to the style minimum,
            // and the size the user actually worked at would be gone.
            st.restoreExtent = node->Size[spec.axis];
        }
        st.splitterDragId = splitterId;
        const float nodeFar = node->Pos[spec.axis] + node->Size[spec.axis];
        st.hideArmed = pure::ShouldArmHide(g.IO.MousePos[spec.axis], nodeFar,
                                           pure::HideArmBandPx(g.Style.WindowMinSize[spec.axis]));
        if (st.hideArmed) {
            DrawReleaseToHideHint(spec, node);
        }
        return;
    }
    if (st.splitterDragId == 0) {
        return;
    }
    // The splitter drag ended this frame. Hide only when it ended in a left-button
    // release while armed (last frame's in-band verdict — the mouse barely moves in one
    // frame); a cancelled drag or one that wandered back out is a plain resize.
    const bool hide = st.hideArmed && ::ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    st.splitterDragId = 0;
    st.hideArmed = false;
    if (hide) {
        CollapseEdgePanel(d, spec);
    }
}

// True when the active item drives a table column drag - a resize handle or a held header
// being reordered (the grid is Reorderable) - both horizontal drags the reveal must not
// steal. The resize handles are not reachable from the window, so every column of every
// table drawn by the active item's window is compared with ImGui's own resize id (instance
// 0 and the table's current instance); the table pool is small, so this is cheap.
bool ActiveItemIsTableColumnDrag(ImGuiContext& g) {
    if (g.ActiveIdWindow == nullptr) {
        return false;
    }
    for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
        ImGuiTable* table = g.Tables.TryGetMapData(n);
        if (table == nullptr || (table->OuterWindow != g.ActiveIdWindow && table->InnerWindow != g.ActiveIdWindow)) {
            continue;
        }
        if (table->HeldHeaderColumn >= 0) {
            return true;
        }
        for (int c = 0; c < table->ColumnsCount; ++c) {
            if (g.ActiveId == ::ImGui::TableGetColumnResizeID(table, c, 0) ||
                g.ActiveId == ::ImGui::TableGetColumnResizeID(table, c, table->InstanceCurrent)) {
                return true;
            }
        }
    }
    return false;
}

// Active items a reveal drag must not steal from (the decision itself is
// pure::ShouldYieldToActiveItem). Everything else (vertical scrollbars, table cells,
// selectables) is stealable once the drag has clearly committed.
bool ActiveItemIsProtected(ImGuiContext& g) {
    if (g.ActiveId == 0) {
        return false;
    }
    ImGuiWindow* w = g.ActiveIdWindow;
    const bool inDockHost = w != nullptr && (w->Flags & ImGuiWindowFlags_DockNodeHost) != 0;
    const bool hScrollbar = w != nullptr && g.ActiveId == ::ImGui::GetWindowScrollbarID(w, ImGuiAxis_X);
    const bool bottomGrip = w != nullptr && w == ::ImGui::FindWindowByName(kBottomSpec.gripWindowName);
    return pure::ShouldYieldToActiveItem(inDockHost, g.InputTextState.ID == g.ActiveId, g.DragDropActive, hScrollbar,
                                         g.MovingWindow != nullptr, ActiveItemIsTableColumnDrag(g), bottomGrip);
}

// Side-column reveal strip along the parent split's right edge. It overlaps the neighbour's
// vertical scrollbar, grid cells and the splitter's grab zone, so it owns NO input up front:
// it is a draw-list overlay that highlights while nothing is active, and a press inside it
// merely ARMS a reveal. The reveal commits once the drag travels clearly leftward
// (RevealDragCommits); whatever item holds the press is released at that point. The items
// that make the strip yield instead are listed at pure::ShouldYieldToActiveItem.
void DrawSideRevealStrip(UiDrawSession& d, const EdgePanelSpec& spec, const ImGuiViewport* vp,
                         const ImGuiDockNode* node, float farEdge) {
    ImGuiContext& g = *::ImGui::GetCurrentContext();
    EdgePanelDragState& st = d.*(spec.state);
    const float gripPx = pure::kRevealGripHeightPx;
    const ImVec2 pos(farEdge - gripPx, node->ParentNode->Pos.y);
    const ImVec2 size(gripPx, node->ParentNode->Size.y);
    const ImVec2 mouse = g.IO.MousePos;
    const bool inStrip = mouse.x >= pos.x && mouse.x < pos.x + size.x && mouse.y >= pos.y && mouse.y < pos.y + size.y;
    const bool yielded = ActiveItemIsProtected(g);
    // Only reachable when the mouse is over the dockspace itself: not through a modal, a
    // floating window or the status bar above it, nor during a window move / drag-and-drop.
    const ImGuiWindow* host = DockHostWindow(node);
    const bool reachable =
        pure::SideStripReachable(::ImGui::GetTopMostPopupModal() != nullptr, g.MovingWindow != nullptr,
                                 g.HoveredWindow != nullptr && host != nullptr &&
                                     g.HoveredWindow->RootWindowDockTree == host->RootWindowDockTree,
                                 g.DragDropActive);
    if (!st.revealArmed && inStrip && reachable && !yielded && ::ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.revealArmed = true;
        st.revealDragStart = mouse.x;
        st.revealDragStartCross = mouse.y;
    }
    if (st.revealArmed) {
        if (yielded || !::ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.revealArmed = false;
        } else if (pure::RevealDragCommits(st.revealDragStart, mouse.x, mouse.y - st.revealDragStartCross)) {
            if (g.ActiveId != 0) {
                ::ImGui::ClearActiveID(); // the item that held the press lets go
            }
            const float extent = pure::RevealHeightForMouseY(mouse.x, farEdge, g.Style.WindowMinSize.x,
                                                             vp->WorkSize.x * pure::kMaxRevealWorkShare);
            ExpandEdgePanel(d, spec, extent);
            st.revealDragActive = true;
            st.revealPeakExtent = extent;
            st.revealArmed = false;
            return;
        }
    }
    // A hovered cell underneath is expected (the strip overlays it); an ACTIVE item is not.
    if ((inStrip && reachable && g.ActiveId == 0) || st.revealArmed) {
        ::ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ::ImGui::GetForegroundDrawList()->AddRectFilled(
            pos, ImVec2(pos.x + size.x, pos.y + size.y),
            ::ImGui::GetColorU32(st.revealArmed ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
        // Don't replace a hovered grid cell's own tooltip.
        if (!st.revealArmed && g.HoveredId == 0) {
            ::ImGui::SetTooltip("%s", SmatchetLocalization::T(spec.revealTipKey, spec.revealTipFallback));
        }
    }
}

void DrawRevealGrip(UiDrawSession& d, const EdgePanelSpec& spec, const ImGuiDockNode* node) {
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    if (vp == nullptr) {
        return;
    }
    float farEdge = 0.0f;
    if (!FarEdge(spec, vp, node, farEdge)) {
        return;
    }
    if (spec.axis == ImGuiAxis_X) {
        // A side column that was never hidden by the gesture / a collapse has nothing to
        // restore: no grip (the user reopens it from the menu).
        if (SideGripCandidate(spec, node, d) && SideGripWinsAmongCoincident(d, spec, vp, farEdge)) {
            DrawSideRevealStrip(d, spec, vp, node, farEdge);
        } else {
            (d.*(spec.state)).revealArmed = false;
        }
        return;
    }
    const float gripPx = pure::kRevealGripHeightPx;
    const ImVec2 pos(vp->WorkPos.x, farEdge - gripPx);
    const ImVec2 size(vp->WorkSize.x, gripPx);
    ::ImGui::SetNextWindowPos(pos);
    ::ImGui::SetNextWindowSize(size);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
    if (::ImGui::Begin(spec.gripWindowName, nullptr, flags)) {
        EdgePanelDragState& st = d.*(spec.state);
        ::ImGui::InvisibleButton("##grip", size);
        const bool hovered = ::ImGui::IsItemHovered();
        const bool active = ::ImGui::IsItemActive();
        if (hovered || active) {
            ::ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ::ImGui::GetWindowDrawList()->AddRectFilled(
                pos, ImVec2(pos.x + size.x, pos.y + size.y),
                ::ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
        }
        if (hovered && !active) {
            ::ImGui::SetTooltip("%s", SmatchetLocalization::T(spec.revealTipKey, spec.revealTipFallback));
        }
        const float mouse = ::ImGui::GetIO().MousePos[spec.axis];
        if (::ImGui::IsItemActivated()) {
            st.revealDragStart = mouse;
        }
        if (active && pure::RevealDragCrossedThreshold(st.revealDragStart, mouse)) {
            const float extent =
                pure::RevealHeightForMouseY(mouse, farEdge, ::ImGui::GetStyle().WindowMinSize[spec.axis],
                                            vp->WorkSize[spec.axis] * pure::kMaxRevealWorkShare);
            ExpandEdgePanel(d, spec, extent);
            st.revealDragActive = true;
            st.revealPeakExtent = extent;
        }
    }
    ::ImGui::End();
    ::ImGui::PopStyleVar(3);
}

void TickRevealFollow(UiDrawSession& d, const EdgePanelSpec& spec, const ImGuiDockNode* node) {
    EdgePanelDragState& st = d.*(spec.state);
    if (!st.revealDragActive) {
        return;
    }
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    float farEdge = 0.0f;
    if (vp == nullptr || !FarEdge(spec, vp, node, farEdge)) {
        st.revealDragActive = false;
        return;
    }
    const float mouse = ::ImGui::GetIO().MousePos[spec.axis];
    if (::ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // The grip strip vanished the moment the panel reopened, so the live resize
        // tracks the raw mouse until release rather than a widget's active state.
        st.pendingExtent = pure::RevealHeightForMouseY(mouse, farEdge, ::ImGui::GetStyle().WindowMinSize[spec.axis],
                                                       vp->WorkSize[spec.axis] * pure::kMaxRevealWorkShare);
        if (st.pendingExtent > st.revealPeakExtent) {
            st.revealPeakExtent = st.pendingExtent;
        }
        if (st.applyFrames < 2) {
            st.applyFrames = 2;
        }
        return;
    }
    st.revealDragActive = false;
    // Symmetry with the splitter gesture: a reveal drag that genuinely opened the panel
    // (peak extent well above the band) and then released back inside the hide band
    // collapses it again instead of leaving a minimum-size sliver. The peak guard
    // matters because the open threshold sits INSIDE the band: without it a minimal
    // open-drag would flash the panel open and instantly re-collapse on release.
    if (pure::ShouldCollapseOnRevealRelease(st.revealPeakExtent, mouse, farEdge,
                                            pure::HideArmBandPx(::ImGui::GetStyle().WindowMinSize[spec.axis]))) {
        CollapseEdgePanel(d, spec);
    }
}

void ApplyPendingExtent(UiDrawSession& d, const EdgePanelSpec& spec, ImGuiDockNode* node) {
    EdgePanelDragState& st = d.*(spec.state);
    if (st.applyFrames <= 0) {
        return;
    }
    --st.applyFrames;
    if (node == nullptr) {
        return; // node not rebuilt yet this frame — the remaining armed frames retry
    }
    ImGuiDockNode* parent = node->ParentNode;
    if (!IsFarChildOnAxis(node, spec.axis) || parent->ChildNodes[0] == nullptr) {
        return;
    }
    // Write BOTH children's SizeRef (what the splitter itself does) so the next
    // DockNodeTreeUpdatePosSize pass reproduces the exact extent instead of
    // proportionally rescaling a stale pair.
    const float minPx = ::ImGui::GetStyle().WindowMinSize[spec.axis];
    const float maxPx = parent->Size[spec.axis] > minPx * 2.0f ? parent->Size[spec.axis] - minPx : minPx;
    const float extent = pure::ClampPx(st.pendingExtent, minPx, maxPx);
    parent->ChildNodes[0]->SizeRef[spec.axis] = parent->Size[spec.axis] - extent;
    node->SizeRef[spec.axis] = extent;
    ::ImGui::MarkIniSettingsDirty();
}

// Returns whether tabs outside the registry (leftovers) keep the panel visible.
bool CollapseEdgePanel(UiDrawSession& d, const EdgePanelSpec& spec) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return false; // no context, no dock tree — nothing to hide (and GetStyle would crash)
    }
    EdgePanelDragState& st = d.*(spec.state);
    ImGuiDockNode* node = EdgePanelNode(spec);
    // Remember only a REAL extent: at gesture release the splitter has already clamped
    // the node to the style minimum, and TickHideGesture recorded the pre-drag extent —
    // a clamped sliver must not overwrite it (the next reveal would lose the user's size).
    if (node != nullptr && node->Size[spec.axis] > ::ImGui::GetStyle().WindowMinSize[spec.axis] * 1.5f) {
        st.restoreExtent = node->Size[spec.axis];
    }
    std::vector<std::string> captured;
    std::vector<const ImGuiWindow*> closed;
    for (size_t i = 0; i < sizeof(kClosableWindows) / sizeof(kClosableWindows[0]); ++i) {
        const ClosableWindowEntry& entry = kClosableWindows[i];
        if (!(d.*(entry.showFlag))) {
            continue;
        }
        ImGuiWindow* w = ::ImGui::FindWindowByName(SmatchetLocalization::WindowTitleFromSource(entry.sourceTitle));
        if (w == nullptr || !WindowInsideNode(w, spec.nodeId)) {
            // Open somewhere else (floating / user-moved / expanded) — hiding the panel
            // must not close it.
            continue;
        }
        captured.push_back(entry.layoutKey);
        closed.push_back(w);
        d.*(entry.showFlag) = false;
    }
    // A capture-less collapse (e.g. an idempotent view.panel.bottom "hide" while the
    // panel is already hidden) keeps the previously remembered tab set — clearing it
    // would degrade the next reveal to the default window.
    if (!captured.empty()) {
        st.restoreKeys = captured;
        st.collapseSeq = ++d.edgePanelCollapseCounter;
    }
    // Windows outside the registry (grid panes, windows the user drag-docked here)
    // cannot be closed from a show-flag table; the panel stays visible with them, so
    // ShowPanel must keep saying so instead of going inconsistent with the screen.
    bool leftovers = false;
    if (node != nullptr) {
        std::vector<const ImGuiWindow*> remaining;
        GatherDockNodeWindows(node, remaining);
        for (const ImGuiWindow* w : remaining) {
            if (std::find(closed.begin(), closed.end(), w) == closed.end()) {
                leftovers = true;
                break;
            }
        }
    }
    if (spec.cfgFlag != nullptr) {
        // A flag shared by several columns (Primary: Views + primary side bar) stays true
        // while ANY sibling column still shows content; CollapseSideBar overrides this with
        // the exact group verdict when it hides the whole group.
        bool anyVisible = leftovers;
        for (const EdgePanelSpec* sibling : kSideSpecs) {
            const ImGuiDockNode* sibNode = EdgePanelNode(*sibling);
            if (sibling != &spec && sibling->cfgFlag == spec.cfgFlag && sibNode != nullptr &&
                NodeShowsContent(sibNode)) {
                anyVisible = true;
            }
        }
        d.cfg.*(spec.cfgFlag) = anyVisible;
    }
    st.revealDragActive = false;
    st.applyFrames = 0;
    if (leftovers && spec.axis == ImGuiAxis_X &&
        st.restoreExtent > ::ImGui::GetStyle().WindowMinSize[spec.axis] * 1.5f) {
        // The column stays (leftover tabs): put it back at its pre-drag width instead of
        // leaving the clamped sliver the splitter drag ended on.
        st.pendingExtent = st.restoreExtent;
        st.applyFrames = 8;
    }
    LOG_DEBUG("%s: collapse closed %d tab(s) (leftovers=%d, restore extent %.0f)", spec.logName,
              static_cast<int>(captured.size()), leftovers ? 1 : 0, st.restoreExtent);
    return leftovers;
}

void ExpandEdgePanel(UiDrawSession& d, const EdgePanelSpec& spec, float desiredExtentPx) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return; // no context, no dock tree — GetMainViewport/GetStyle below need one
    }
    EdgePanelDragState& st = d.*(spec.state);
    if (st.restoreKeys.empty() && spec.defaultRestoreKey != nullptr) {
        st.restoreKeys.push_back(spec.defaultRestoreKey); // nothing remembered (fresh session)
    }
    bool focusGiven = false;
    for (size_t i = 0; i < sizeof(kClosableWindows) / sizeof(kClosableWindows[0]); ++i) {
        const ClosableWindowEntry& entry = kClosableWindows[i];
        if (std::find(st.restoreKeys.begin(), st.restoreKeys.end(), entry.layoutKey) == st.restoreKeys.end()) {
            continue;
        }
        d.*(entry.showFlag) = true;
        if (!focusGiven && entry.focusFlag != nullptr) {
            d.*(entry.focusFlag) = true; // raise one tab so the reopened panel has a selection
            focusGiven = true;
        }
    }
    st.restoreKeys.clear();
    if (spec.cfgFlag != nullptr) {
        d.cfg.*(spec.cfgFlag) = true;
    }
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    const float workExtent = vp != nullptr ? vp->WorkSize[spec.axis] : 0.0f;
    const float minPx = ::ImGui::GetStyle().WindowMinSize[spec.axis];
    st.pendingExtent =
        desiredExtentPx > 0.0f ? desiredExtentPx : pure::RestoreHeight(st.restoreExtent, minPx, workExtent);
    st.applyFrames = 8; // settle window — mirrors layoutForceDefaultsFrames
    LOG_DEBUG("%s: expanded (extent %.0f)", spec.logName, st.pendingExtent);
}

// Drop remembered tabs whose window has been reopened some other way (see
// pure::ShouldKeepRestoreKey). Expand clears the set itself, so a reveal in progress and the
// leftovers case (windows still closed) are unaffected.
void PruneRestoreKeys(UiDrawSession& d, const EdgePanelSpec& spec) {
    std::vector<std::string>& keys = (d.*(spec.state)).restoreKeys;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&d](const std::string& key) {
                                  for (const ClosableWindowEntry& entry : kClosableWindows) {
                                      if (key == entry.layoutKey) {
                                          return !pure::ShouldKeepRestoreKey(d.*(entry.showFlag));
                                      }
                                  }
                                  return false;
                              }),
               keys.end());
}

void TickEdgePanel(UiDrawSession& d, const EdgePanelSpec& spec) {
    PruneRestoreKeys(d, spec);
    const EdgePanelDragState& st = d.*(spec.state);
    ImGuiDockNode* node = EdgePanelNode(spec);
    TickRevealFollow(d, spec, node);
    ApplyPendingExtent(d, spec, node);
    if (node != nullptr && NodeShowsContent(node)) {
        (d.*(spec.state)).revealArmed = false; // no grip is drawn: a stale arm must not commit a later press
        TickHideGesture(d, spec, node);
    } else if (!st.revealDragActive) {
        DrawRevealGrip(d, spec, node);
    } else {
        (d.*(spec.state)).revealArmed = false;
    }
}

} // namespace

namespace SmatchetBottomPanelDrag {

bool IsPanelVisible() {
    const ImGuiDockNode* node = EdgePanelNode(kBottomSpec);
    return node != nullptr && NodeShowsContent(node);
}

void Collapse(UiDrawSession& d) { CollapseEdgePanel(d, kBottomSpec); }

void Expand(UiDrawSession& d, float desiredHeightPx) { ExpandEdgePanel(d, kBottomSpec, desiredHeightPx); }

namespace {

// The columns each View > Appearance side bar covers.
const EdgePanelSpec* const kPrimaryBarSpecs[] = {&kViewsSpec, &kPrimarySideSpec};
const EdgePanelSpec* const kSecondaryBarSpecs[] = {&kSecondarySideSpec};

bool BarSpecs(SmatchetBottomPanelDrag::SideBar bar, const EdgePanelSpec* const*& specs, size_t& count) {
    if (bar == SmatchetBottomPanelDrag::SideBar::Primary) {
        specs = kPrimaryBarSpecs;
        count = sizeof(kPrimaryBarSpecs) / sizeof(kPrimaryBarSpecs[0]);
        return true;
    }
    specs = kSecondaryBarSpecs;
    count = sizeof(kSecondaryBarSpecs) / sizeof(kSecondaryBarSpecs[0]);
    return false;
}

} // namespace

bool IsSideBarVisible(SideBar bar) {
    const EdgePanelSpec* const* specs = nullptr;
    size_t count = 0;
    BarSpecs(bar, specs, count);
    for (size_t i = 0; i < count; ++i) {
        const ImGuiDockNode* node = EdgePanelNode(*specs[i]);
        if (node != nullptr && NodeShowsContent(node)) {
            return true;
        }
    }
    return false;
}

void CollapseSideBar(UiDrawSession& d, SideBar bar) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const EdgePanelSpec* const* specs = nullptr;
    size_t count = 0;
    BarSpecs(bar, specs, count);
    bool anyLeftovers = false;
    for (size_t i = 0; i < count; ++i) {
        const ImGuiDockNode* node = EdgePanelNode(*specs[i]);
        if (node != nullptr && NodeShowsContent(node)) {
            anyLeftovers = CollapseEdgePanel(d, *specs[i]) || anyLeftovers;
        }
    }
    d.cfg.*(specs[0]->cfgFlag) = pure::SideBarFlagAfterCollapse(anyLeftovers);
}

namespace {

bool AnyRemembered(const UiDrawSession& d, const EdgePanelSpec* const* specs, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!(d.*(specs[i]->state)).restoreKeys.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

bool CanExpandSideBar(const UiDrawSession& d, SideBar bar) {
    const EdgePanelSpec* const* specs = nullptr;
    size_t count = 0;
    const bool primary = BarSpecs(bar, specs, count);
    return pure::PlanSideBarShow(AnyRemembered(d, specs, count), primary && !d.showViewsDashboard) !=
           pure::SideBarShowPlan::Nothing;
}

bool ExpandSideBar(UiDrawSession& d, SideBar bar) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return false; // no dock tree: nothing can open, and the command must not claim it did
    }
    const EdgePanelSpec* const* specs = nullptr;
    size_t count = 0;
    BarSpecs(bar, specs, count);
    // The Views fallback only helps when the dashboard is closed; if it is already open
    // (floating / docked elsewhere) there is nothing to open, so report that honestly.
    const pure::SideBarShowPlan plan =
        pure::PlanSideBarShow(AnyRemembered(d, specs, count), bar == SideBar::Primary && !d.showViewsDashboard);
    if (plan == pure::SideBarShowPlan::Nothing) {
        return false;
    }
    if (plan == pure::SideBarShowPlan::FallbackToViews) {
        (d.*(kViewsSpec.state)).restoreKeys.push_back("views");
        ExpandEdgePanel(d, kViewsSpec, 0.0f);
        return true;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!(d.*(specs[i]->state)).restoreKeys.empty()) {
            ExpandEdgePanel(d, *specs[i], 0.0f);
        }
    }
    return true;
}

void Tick(UiDrawSession& d) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    TickEdgePanel(d, kBottomSpec);
    for (const EdgePanelSpec* side : kSideSpecs) {
        TickEdgePanel(d, *side);
    }
}

} // namespace SmatchetBottomPanelDrag
