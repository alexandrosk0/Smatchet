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
#include <string>
#include <vector>

namespace pure = SmatchetBottomPanelDragPure;

namespace {

// One row per window that docks into the bottom panel by default (mirrors the
// kBottomPanel rows of SmatchetDockNodeIds' layout-key table). sourceTitle is the
// English source title handed to Begin(); FindWindowByName goes through the same
// WindowTitleFromSource transform the localized Begin wrapper applies, so lookups hit
// in every locale (the selectDockedTab pattern).
struct BottomWindowEntry {
    const char* layoutKey;
    const char* sourceTitle;
    bool UiDrawSession::*showFlag;
    bool UiDrawSession::*focusFlag; // null when the window has no focus latch
};

const BottomWindowEntry kBottomWindows[] = {
    {"log", "Log", &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus},
    {"preferences", "Preferences", &UiDrawSession::showPreferences, &UiDrawSession::requestPreferencesFocus},
    {"performance", "Performance", &UiDrawSession::showPerformance, &UiDrawSession::requestPerformanceFocus},
    {"audit", "Backend Audit", &UiDrawSession::showAuditTrail, &UiDrawSession::requestAuditTrailFocus},
    {"bulk_import", "Bulk Import Issues", &UiDrawSession::showBulkImport, &UiDrawSession::requestBulkImportFocus},
    {"bulk_export", "Bulk export tickets", &UiDrawSession::showBulkExport, &UiDrawSession::requestBulkExportFocus},
    {"annotate", "Annotate###AnnotateAnalysisModal", &UiDrawSession::showAnnotateAnalysis, nullptr},
    {"plandocs", "Plan Docs", &UiDrawSession::showPlanDocViewer, &UiDrawSession::requestPlanDocViewerFocus},
    {"user_info", "User Info", &UiDrawSession::showUserInfo, &UiDrawSession::requestUserInfoFocus},
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    {"scripting", "Scripting", &UiDrawSession::showLuaAutomationWindow, &UiDrawSession::requestLuaAutomationFocus},
#endif
#if defined(SMATCHET_WITH_MCP)
    {"mcp", "MCP Server", &UiDrawSession::showMcpServerWindow, &UiDrawSession::requestMcpServerFocus},
#endif
};

ImGuiDockNode* BottomPanelNode() {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return nullptr;
    }
    ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(SmatchetDockNodeIds::kBottomPanel);
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

ImGuiWindow* DockHostWindow(ImGuiDockNode* node) {
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

bool WindowInsideBottomPanel(const ImGuiWindow* w) {
    for (const ImGuiDockNode* n = w->DockNode; n != nullptr; n = n->ParentNode) {
        if (n->ID == SmatchetDockNodeIds::kBottomPanel) {
            return true;
        }
    }
    return false;
}

// Mirrors DockNodeTreeUpdateSplitter (imgui.cpp): the splitter between a split node's
// two children is submitted in the dock host window under PushID((int)parent->ID) +
// GetID("##Splitter"), and the host window's ID stack holds only its own ID at that
// point. Reproducing the id pins "the active drag IS the splitter directly above the
// bottom panel" — no cursor or geometry heuristics, no false positives from other
// splitters or scrollbar drags near the bottom edge.
ImGuiID SplitterAboveBottomPanelId(ImGuiDockNode* node) {
    ImGuiDockNode* parent = node->ParentNode;
    ImGuiWindow* host = DockHostWindow(node);
    if (parent == nullptr || host == nullptr) {
        return 0;
    }
    // Only the "panel is the lower child of a vertical split" arrangement has a
    // splitter whose drag-down means "shrink the panel"; any other arrangement (the
    // user moved the node) just disables the gesture.
    if (parent->SplitAxis != ImGuiAxis_Y || parent->ChildNodes[1] != node) {
        return 0;
    }
    const int parentIdBits = static_cast<int>(parent->ID); // PushID(int) hashes the raw int bytes
    const ImGuiID seed = ImHashData(&parentIdBits, sizeof(parentIdBits), host->ID);
    return ImHashStr("##Splitter", 0, seed);
}

void DrawReleaseToHideHint(const ImGuiDockNode* node) {
    ImDrawList* dl = ::ImGui::GetForegroundDrawList();
    const ImVec2 rectMin = node->Pos;
    const ImVec2 rectMax(node->Pos.x + node->Size.x, node->Pos.y + node->Size.y);
    dl->AddRectFilled(rectMin, rectMax, ::ImGui::GetColorU32(ImGuiCol_DockingPreview, 0.35f));
    const char* hint = SmatchetLocalization::T("panel.release_to_hide", "Release to hide the panel");
    const ImVec2 textSize = ::ImGui::CalcTextSize(hint);
    float textY = rectMin.y + (node->Size.y - textSize.y) * 0.5f;
    if (node->Size.y < textSize.y + 8.0f) {
        textY = rectMin.y - textSize.y - 4.0f; // panel squeezed to a sliver — hint above it
    }
    dl->AddText(ImVec2(rectMin.x + (node->Size.x - textSize.x) * 0.5f, textY), ::ImGui::GetColorU32(ImGuiCol_Text),
                hint);
}

void TickHideGesture(UiDrawSession& d, ImGuiDockNode* node) {
    ImGuiContext& g = *::ImGui::GetCurrentContext();
    const ImGuiID splitterId = SplitterAboveBottomPanelId(node);
    if (splitterId != 0 && g.ActiveId == splitterId) {
        if (d.bottomPanelSplitterDragId == 0 && node->Size.y > g.Style.WindowMinSize.y * 1.5f) {
            // Drag start: capture the pre-drag height NOW. By the release that hides
            // the panel the splitter has long clamped the node to the style minimum,
            // and the height the user actually worked at would be gone.
            d.bottomPanelRestoreHeight = node->Size.y;
        }
        d.bottomPanelSplitterDragId = splitterId;
        const float nodeBottom = node->Pos.y + node->Size.y;
        d.bottomPanelHideArmed =
            pure::ShouldArmHide(g.IO.MousePos.y, nodeBottom, pure::HideArmBandPx(g.Style.WindowMinSize.y));
        if (d.bottomPanelHideArmed) {
            DrawReleaseToHideHint(node);
        }
        return;
    }
    if (d.bottomPanelSplitterDragId == 0) {
        return;
    }
    // The splitter drag ended this frame. Hide only when it ended in a left-button
    // release while armed (last frame's in-band verdict — the mouse barely moves in one
    // frame); a cancelled drag or one that wandered back up is a plain resize.
    const bool hide = d.bottomPanelHideArmed && ::ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    d.bottomPanelSplitterDragId = 0;
    d.bottomPanelHideArmed = false;
    if (hide) {
        SmatchetBottomPanelDrag::Collapse(d);
    }
}

void DrawRevealGrip(UiDrawSession& d) {
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    if (vp == nullptr) {
        return;
    }
    const float gripH = 7.0f;
    const ImVec2 pos(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - gripH);
    const ImVec2 size(vp->WorkSize.x, gripH);
    ::ImGui::SetNextWindowPos(pos);
    ::ImGui::SetNextWindowSize(size);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
    if (::ImGui::Begin("##BottomPanelRevealGrip", nullptr, flags)) {
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
            ::ImGui::SetTooltip("%s", SmatchetLocalization::T("panel.drag_to_open", "Drag up to open the panel"));
        }
        if (::ImGui::IsItemActivated()) {
            d.bottomPanelRevealDragStartY = ::ImGui::GetIO().MousePos.y;
        }
        if (active && pure::RevealDragCrossedThreshold(d.bottomPanelRevealDragStartY, ::ImGui::GetIO().MousePos.y)) {
            const float workBottom = vp->WorkPos.y + vp->WorkSize.y;
            const float h = pure::RevealHeightForMouseY(::ImGui::GetIO().MousePos.y, workBottom,
                                                        ::ImGui::GetStyle().WindowMinSize.y,
                                                        vp->WorkSize.y * pure::kMaxRevealWorkShare);
            SmatchetBottomPanelDrag::Expand(d, h);
            d.bottomPanelRevealDragActive = true;
        }
    }
    ::ImGui::End();
    ::ImGui::PopStyleVar(3);
}

void TickRevealFollow(UiDrawSession& d) {
    if (!d.bottomPanelRevealDragActive) {
        return;
    }
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    if (vp == nullptr) {
        d.bottomPanelRevealDragActive = false;
        return;
    }
    const float workBottom = vp->WorkPos.y + vp->WorkSize.y;
    const float mouseY = ::ImGui::GetIO().MousePos.y;
    if (::ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // The grip strip vanished the moment the panel reopened, so the live resize
        // tracks the raw mouse until release rather than a widget's active state.
        d.bottomPanelPendingHeight = pure::RevealHeightForMouseY(
            mouseY, workBottom, ::ImGui::GetStyle().WindowMinSize.y, vp->WorkSize.y * pure::kMaxRevealWorkShare);
        if (d.bottomPanelApplyHeightFrames < 2) {
            d.bottomPanelApplyHeightFrames = 2;
        }
        return;
    }
    d.bottomPanelRevealDragActive = false;
    // Symmetry with the splitter gesture: a reveal drag released back down inside the
    // hide band collapses the panel again instead of leaving a minimum-height sliver.
    if (pure::ShouldArmHide(mouseY, workBottom, pure::HideArmBandPx(::ImGui::GetStyle().WindowMinSize.y))) {
        SmatchetBottomPanelDrag::Collapse(d);
    }
}

void ApplyPendingHeight(UiDrawSession& d, ImGuiDockNode* node) {
    if (d.bottomPanelApplyHeightFrames <= 0) {
        return;
    }
    --d.bottomPanelApplyHeightFrames;
    if (node == nullptr) {
        return; // node not rebuilt yet this frame — the remaining armed frames retry
    }
    ImGuiDockNode* parent = node->ParentNode;
    if (parent == nullptr || parent->SplitAxis != ImGuiAxis_Y || parent->ChildNodes[1] != node ||
        parent->ChildNodes[0] == nullptr) {
        return;
    }
    // Write BOTH children's SizeRef (what the splitter itself does) so the next
    // DockNodeTreeUpdatePosSize pass reproduces the exact height instead of
    // proportionally rescaling a stale pair.
    const float minY = ::ImGui::GetStyle().WindowMinSize.y;
    const float maxH = parent->Size.y > minY * 2.0f ? parent->Size.y - minY : minY;
    const float h = pure::ClampPx(d.bottomPanelPendingHeight, minY, maxH);
    parent->ChildNodes[0]->SizeRef.y = parent->Size.y - h;
    node->SizeRef.y = h;
    ::ImGui::MarkIniSettingsDirty();
}

} // namespace

namespace SmatchetBottomPanelDrag {

bool IsPanelVisible() {
    ImGuiDockNode* node = BottomPanelNode();
    return node != nullptr && NodeShowsContent(node);
}

void Collapse(UiDrawSession& d) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return; // no context, no dock tree — nothing to hide (and GetStyle would crash)
    }
    ImGuiDockNode* node = BottomPanelNode();
    // Remember only a REAL height: at gesture release the splitter has already clamped
    // the node to the style minimum, and TickHideGesture recorded the pre-drag height —
    // a clamped sliver must not overwrite it (the next reveal would lose the user's size).
    if (node != nullptr && node->Size.y > ::ImGui::GetStyle().WindowMinSize.y * 1.5f) {
        d.bottomPanelRestoreHeight = node->Size.y;
    }
    std::vector<std::string> captured;
    std::vector<const ImGuiWindow*> closed;
    for (const BottomWindowEntry& entry : kBottomWindows) {
        if (!(d.*(entry.showFlag))) {
            continue;
        }
        ImGuiWindow* w = ::ImGui::FindWindowByName(SmatchetLocalization::WindowTitleFromSource(entry.sourceTitle));
        if (w == nullptr || !WindowInsideBottomPanel(w)) {
            // Open somewhere else (floating / user-moved) — hiding the panel must not
            // close it.
            continue;
        }
        captured.push_back(entry.layoutKey);
        closed.push_back(w);
        d.*(entry.showFlag) = false;
    }
    // A capture-less collapse (e.g. an idempotent view.panel.bottom "hide" while the
    // panel is already hidden) keeps the previously remembered tab set — clearing it
    // would degrade the next reveal to the Log fallback.
    if (!captured.empty()) {
        d.bottomPanelRestoreKeys = captured;
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
    d.cfg.ShowPanel = leftovers;
    d.bottomPanelRevealDragActive = false;
    d.bottomPanelApplyHeightFrames = 0;
    LOG_DEBUG("BottomPanel: collapse closed %d tab(s) (leftovers=%d, restore height %.0f)",
              static_cast<int>(captured.size()), leftovers ? 1 : 0, d.bottomPanelRestoreHeight);
}

void Expand(UiDrawSession& d, float desiredHeightPx) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return; // no context, no dock tree — GetMainViewport/GetStyle below need one
    }
    if (d.bottomPanelRestoreKeys.empty()) {
        d.bottomPanelRestoreKeys.push_back("log"); // nothing remembered (fresh session) — reveal the Log
    }
    bool focusGiven = false;
    for (const BottomWindowEntry& entry : kBottomWindows) {
        if (std::find(d.bottomPanelRestoreKeys.begin(), d.bottomPanelRestoreKeys.end(), entry.layoutKey) ==
            d.bottomPanelRestoreKeys.end()) {
            continue;
        }
        d.*(entry.showFlag) = true;
        if (!focusGiven && entry.focusFlag != nullptr) {
            d.*(entry.focusFlag) = true; // raise one tab so the reopened panel has a selection
            focusGiven = true;
        }
    }
    d.bottomPanelRestoreKeys.clear();
    d.cfg.ShowPanel = true;
    const ImGuiViewport* vp = ::ImGui::GetMainViewport();
    const float workH = vp != nullptr ? vp->WorkSize.y : 0.0f;
    const float minY = ::ImGui::GetStyle().WindowMinSize.y;
    d.bottomPanelPendingHeight =
        desiredHeightPx > 0.0f ? desiredHeightPx : pure::RestoreHeight(d.bottomPanelRestoreHeight, minY, workH);
    d.bottomPanelApplyHeightFrames = 8; // settle window — mirrors layoutForceDefaultsFrames
    LOG_DEBUG("BottomPanel: expanded (height %.0f)", d.bottomPanelPendingHeight);
}

void Tick(UiDrawSession& d) {
    if (::ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiDockNode* node = BottomPanelNode();
    TickRevealFollow(d);
    ApplyPendingHeight(d, node);
    if (node != nullptr && NodeShowsContent(node)) {
        TickHideGesture(d, node);
    } else if (!d.bottomPanelRevealDragActive) {
        DrawRevealGrip(d);
    }
}

} // namespace SmatchetBottomPanelDrag
