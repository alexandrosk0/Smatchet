#include "SmatchetUI_Internal.h"

class AppController; // fan-in Phase 6: this TU only names AppController& in signatures / pass-throughs — fwd-decl
                     // suffices
#include "ConfigManager.h"
#include "ConfigSaveWorker.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <cstring>
#include <future>

#include <nlohmann/json.hpp>

namespace {

struct LayoutRect {
    ImVec2 Pos;
    ImVec2 Size;
};

float ClampFloat(float v, float lo, float hi) { return (std::max)(lo, (std::min)(v, hi)); }

LayoutRect DefaultLayoutRectFor(const char* key, float defaultW, float defaultH) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 workPos = vp ? vp->WorkPos : ImVec2(0.0f, 0.0f);
    const ImVec2 workSize = vp ? vp->WorkSize : ImGui::GetIO().DisplaySize;
    const float w = (std::max)(320.0f, workSize.x);
    const float h = (std::max)(240.0f, workSize.y);

    auto centered = [&](float rw, float rh) {
        rw = ClampFloat(rw, 320.0f, w * 0.92f);
        rh = ClampFloat(rh, 240.0f, h * 0.88f);
        return LayoutRect{ImVec2(workPos.x + (w - rw) * 0.5f, workPos.y + (h - rh) * 0.5f), ImVec2(rw, rh)};
    };

    const float sideW = ClampFloat(w * 0.13f, 250.0f, 360.0f);
    const float mainW = (std::max)(420.0f, w - sideW);
    const float prefsH = g_ui.showPreferences ? ClampFloat(h * 0.36f, 300.0f, 470.0f) : 0.0f;
    const float activeH = (std::max)(300.0f, h - prefsH);

    if (std::strcmp(key, "active") == 0) {
        return LayoutRect{workPos, ImVec2(mainW, activeH)};
    }
    if (std::strcmp(key, "views") == 0) {
        return LayoutRect{ImVec2(workPos.x + w - sideW, workPos.y), ImVec2(sideW, h)};
    }
    if (std::strcmp(key, "preferences") == 0) {
        return LayoutRect{ImVec2(workPos.x, workPos.y + activeH), ImVec2(mainW, prefsH)};
    }
    if (std::strcmp(key, "log") == 0) {
        const float logH = ClampFloat(h * 0.34f, 260.0f, 420.0f);
        return LayoutRect{ImVec2(workPos.x, workPos.y + h - logH), ImVec2(w, logH)};
    }
    if (std::strcmp(key, "scripting") == 0 || std::strcmp(key, "mcp") == 0) {
        const float utilityPanelW = ClampFloat(defaultW, 420.0f, (std::min)(720.0f, w * 0.46f));
        const float sideH = ClampFloat(defaultH, 420.0f, h * 0.88f);
        return LayoutRect{ImVec2(workPos.x + w - utilityPanelW - 24.0f, workPos.y + 48.0f),
                          ImVec2(utilityPanelW, sideH)};
    }
    return centered(defaultW, defaultH);
}

bool WindowNeedsRepair(const ImVec2& pos, const ImVec2& size, float minW, float minH) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return false;
    }
    const ImVec2 workMin = vp->WorkPos;
    const ImVec2 workMax(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    if (size.x < minW || size.y < minH) {
        return true;
    }
    if (pos.x > workMax.x - 96.0f || pos.y > workMax.y - 72.0f) {
        return true;
    }
    return pos.x + size.x < workMin.x + 96.0f || pos.y + size.y < workMin.y + 72.0f;
}

} // namespace

namespace smatchet {
namespace ui_detail {

void PersistWindowOpenPreferences(UiDrawSession& d) {
    bool changed = false;
    auto setBool = [&changed](bool& dst, bool src) {
        if (dst != src) {
            dst = src;
            changed = true;
        }
    };
    setBool(d.cfg.ShowPreferencesWindow, d.showPreferences);
    setBool(d.cfg.ShowViewsDashboardWindow, d.showViewsDashboard);
    setBool(d.cfg.ShowPerformanceWindow, d.showPerformance);
    setBool(d.cfg.ShowLogWindow, d.showLogWindow);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    setBool(d.cfg.ShowLuaAutomationWindow, d.showLuaAutomationWindow);
#endif
#if defined(SMATCHET_WITH_MCP)
    setBool(d.cfg.ShowMcpServerWindow, d.showMcpServerWindow);
#endif
    if (changed) {
        ConfigManager::Save(d.cfg);
    }
}

} // namespace ui_detail
} // namespace smatchet

void SmatchetUI::prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                       bool requestFocus) {
    // EnsureDockSlotAlive collapses a registered-but-dead slot to 0, so the branch below is
    // skipped rather than docking into an orphan node that only LOOKS like the real panel.
    const ImGuiID slot =
        SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::DefaultDockSlotForLayoutKey(layoutKey));
    if (slot != 0) {
        // Re-force the redock EVERY frame while the reset is settling (mirrors
        // SmatchetMcpServerUi.cpp). The bare erase() consumes the arm once on the reset
        // frame, but that frame the freshly LoadIni'd dock nodes are not yet marked
        // LastFrameAlive (DockSpaceOverViewport already walked the OLD tree this frame), so
        // BeginDocked's liveness guard (imgui.cpp ~21208) silently undocks the window. The
        // node only becomes alive next frame; without a re-force the window stays floating,
        // masked by repairTopLevelWindow's fallback-rect snap. erase()+repair's re-insert
        // oscillation is not enough: a window whose Begin() returns false (collapsed / unselected
        // bottom-panel tab) never runs repair, so its arm is never re-inserted after the consume.
        const bool needsForce = d.pendingReDockWindows.erase(layoutKey) > 0 || d.layoutForceDefaultsFrames > 0;
        if (needsForce) {
            ImGui::SetNextWindowDockID(slot, ImGuiCond_Always);
        } else if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            // Skip on drag AND release frames: DockId is 0 while floating, so FirstUseEver
            // would override the drop target before ImGui finalises it.
            ImGui::SetNextWindowDockID(slot, ImGuiCond_FirstUseEver);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(defaultW, defaultH), ImGuiCond_FirstUseEver);
    if (requestFocus) {
        ImGui::SetNextWindowFocus();
    }
}

void SmatchetUI::selectDockedTab(const char* windowName) {
    // ImGui::SetNextWindowFocus() does NOT activate a docked tab: FocusWindow()'s
    // "select in dock node" lines are commented out upstream (imgui.cpp, issue #2304),
    // and DockNodeUpdateTabBar only mirrors nav focus when g.NavWindow->RootWindow IS
    // the node's own window — never true for a window docked into a host node. The tab
    // bar therefore keeps whatever selection it picked on the frame it was CREATED
    // (node->SelectedTabId if it resolves, else the last-added tab), which is why a
    // window sharing a node with a sibling could come up behind it and stay there for
    // the rest of the session. Write the selection explicitly instead.
    // Callers pass the SOURCE title they hand to Begin(); the real ImGui window name is
    // the localized one (SmatchetLocalizedImGui::Begin runs the same transform), so a raw
    // lookup on the English literal would miss in every non-English locale.
    ImGuiWindow* window = ImGui::FindWindowByName(SmatchetLocalization::WindowTitleFromSource(windowName));
    if (window == nullptr || window->DockNode == nullptr || window->DockNode->TabBar == nullptr) {
        // Not docked (floating / first frame before Begin) — SetNextWindowFocus suffices.
        return;
    }
    window->DockNode->TabBar->NextSelectedTabId = window->TabId;
    // Selecting the tab is not enough: the node's tab-bar chrome renders in the
    // UNFOCUSED palette (ImGuiCol_TabDimmed*) unless a window inside the node holds
    // nav focus. SetNextWindowFocus() cannot supply that either — it is consumed by
    // the docked child's Begin(), which runs AFTER DockNodeUpdateTabBar has already
    // picked this frame's colours. Focus the window directly so the node is focused
    // from the next frame on (the request is re-armed every warm-up frame).
    ImGui::FocusWindow(window);
}

void SmatchetUI::repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH) {
    if (ImGui::IsWindowDocked()) {
        return;
    }
    // Window is floating — schedule force-dock on next frame if it has a LIVE registered slot.
    // Arming against a dead node would return early forever and leave the off-screen/degenerate
    // rect repair below unreachable, while prepareTopLevelWindow declines to act on the arm:
    // the window would sit floating at a broken rect with nothing left to fix it.
    const ImGuiID slot =
        SmatchetDockNodeIds::EnsureDockSlotAlive(SmatchetDockNodeIds::DefaultDockSlotForLayoutKey(layoutKey));
    if (slot != 0 && !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        d.pendingReDockWindows.insert(layoutKey);
        return;
    }

    const ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    if (!WindowNeedsRepair(pos, size, minW, minH) && d.layoutForceDefaultsFrames <= 0) {
        return;
    }

    const LayoutRect fallback = DefaultLayoutRectFor(layoutKey, minW, minH);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        ImGui::SetWindowPos(fallback.Pos, ImGuiCond_Always);
        ImGui::SetWindowSize(fallback.Size, ImGuiCond_Always);
        return;
    }

    size.x = ClampFloat((std::max)(size.x, minW), minW, (std::max)(minW, vp->WorkSize.x * 0.96f));
    size.y = ClampFloat((std::max)(size.y, minH), minH, (std::max)(minH, vp->WorkSize.y * 0.94f));
    ImVec2 nextPos = pos;
    if (WindowNeedsRepair(pos, size, minW, minH) || d.layoutForceDefaultsFrames > 0) {
        nextPos = fallback.Pos;
        size = fallback.Size;
    }

    const ImVec2 workMin = vp->WorkPos;
    const ImVec2 workMax(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    nextPos.x = ClampFloat(nextPos.x, workMin.x, (std::max)(workMin.x, workMax.x - size.x));
    nextPos.y = ClampFloat(nextPos.y, workMin.y, (std::max)(workMin.y, workMax.y - size.y));
    ImGui::SetWindowPos(nextPos, ImGuiCond_Always);
    ImGui::SetWindowSize(size, ImGuiCond_Always);
}

void SmatchetUI::resetWindowLayoutToDefault(UiDrawSession& d) {
    // Delegates to the free function (single source of truth). The reset body touches only
    // `d` + globals, never `this`, so this member exists purely for callers holding a
    // SmatchetUI*. Keep the two in lock-step by forwarding rather than copying.
    SmatchetUI_ResetLayoutToDefault(d);
}

void SmatchetUI_ResetLayoutToDefault(UiDrawSession& d) {
    // Reset is always requested MID-FRAME — from the menu bar draw (SmatchetUI_MainMenu.cpp)
    // or from a command handler draining on the UI thread (BuiltinCommands_Debug.cpp), both of
    // which run before drawSecondaryWindows / drawEndOfFramePersistence in the frame. Doing the
    // heavy dock work inline corrupts the LIVE dock tree: a mid-frame LoadIniSettingsFromMemory
    // queues the node-tree rebuild for the NEXT NewFrame, but the same-frame force-redock then
    // runs against the stale/half-loaded tree and orphans nodes 0x02/0x04/0x08 (empirically
    // reproduced — Smatchet-31412.log). So only LATCH here; drawEndOfFramePersistence drains the
    // latch via SmatchetUI_ApplyDeferredLayoutReset at end-of-frame, the timing that rebuilds the
    // correct nested tree (Smatchet-50156.log). Keep this the single mid-frame entry point.
    d.pendingLayoutReset = true;
}

void SmatchetUI_ApplyDeferredLayoutReset(UiDrawSession& d) {
    if (!d.pendingLayoutReset) {
        return;
    }
    d.pendingLayoutReset = false;

    // Runs at end-of-frame (drawEndOfFramePersistence). Must be called on the UI thread.
    d.showViewsDashboard = true;
    d.requestActiveProjectFocus = false;
    d.requestViewsDashboardFocus = false;
    d.showPerformance = false;
    d.showAnnotateAnalysis = false;
    d.showBulkImport = false;
    d.showBulkExport = false;
    d.showAuditTrail = false;
    d.showLogWindow = false;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    d.showLuaAutomationWindow = false;
    d.requestLuaAutomationFocus = false;
    d.requestScriptingEditorTabFocus = false;
#endif
#if defined(SMATCHET_WITH_MCP)
    d.showMcpServerWindow = false;
    d.requestMcpServerFocus = false;
#endif

    // Immediate-effect reset (no restart). Three coordinated steps:
    //  1. Rebuild the dock-node tree LIVE from the canonical default. ImGui parses the
    //     embedded default ini and recreates the fixed-ID nodes (0x02 central, 0x08 views,
    //     0x0A bottom, …) with their default split geometry at the next NewFrame. We feed
    //     the embedded string (single source of truth) rather than reading back the file we
    //     are about to write, so there is no flush/read race.
    //  2. Persist that same default to disk so the next launch loads it too.
    ImGui::LoadIniSettingsFromMemory(ConfigManager::GetDefaultImGuiDockLayoutIni());
    ConfigManager::WriteDefaultImGuiSettingsFile();

    //  3. Force-re-parent already-created windows. LoadIniSettings rebuilds node metadata
    //     but does NOT re-dock live windows — they keep their old DockId and float free.
    //     Two triggers cooperate: the layoutForceDefaultsFrames countdown below makes
    //     prepareTopLevelWindow() re-issue SetNextWindowDockID(slot, ImGuiCond_Always) EVERY
    //     frame for ~8 frames (the durable path — it lands once the rebuilt nodes become
    //     LastFrameAlive next frame, and covers windows whose Begin() returns false so their
    //     repair pass never runs); the pendingReDockWindows arm below additionally re-docks
    //     windows that are currently CLOSED, harmlessly waiting in the set until they next
    //     open (possibly long after the 8-frame window expires) so they also land home.
    d.pendingReDockWindows.clear();
    const size_t keyCount = SmatchetDockNodeIds::DefaultDockLayoutKeyCount();
    for (size_t i = 0; i < keyCount; ++i) {
        d.pendingReDockWindows.insert(SmatchetDockNodeIds::DefaultDockLayoutKeyAt(i));
    }

    //  3b. The AI Assistant panel docks through its own ApplyAssistantDocking() path, NOT the
    //      pendingReDockWindows list above. The default tree has no secondary side bar node
    //      (0x10 is created only when the user swaps the panel right), so a reset returns the
    //      panel to its default primary (left) side bar and arms a side-swap so the open panel
    //      snaps home next frame (a closed panel docks correctly when it next opens). Persist
    //      the side/visibility preferences so the next launch agrees with the reset layout.
    //      `assistantPendingSideSwap` only exists when the assistant is compiled in.
#if defined(SMATCHET_WITH_AI)
    d.cfg.AssistantPanelOnSecondarySide = false;
    d.cfg.ShowSecondarySideBar = false;
    d.assistantPendingSideSwap = true;
    smatchet::config_save::EnqueueTrackerConfig(d.cfg);
#endif

    // Keep ImGui auto-save ON: the live tree now equals the default, so the periodic save
    // and the DestroyContext save both write the correct layout. Run the force-dock/repair
    // pass for a few frames so any floating straggler snaps home and the settled default is
    // saved by SmatchetUI.cpp's end-of-frame SaveIniSettingsToDisk when the counter hits 0.
    d.layoutForceDefaultsFrames = 8;

    SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.layout_reset", "Layout reset"),
                                          SmatchetLocalization::T("toast.layout_restored", "Default layout restored."),
                                          ToastType::Info, 3000);
    smatchet::ui_detail::PersistWindowOpenPreferences(d);
}

namespace {

template <typename T> void DrainFutureJoinQuiet(std::future<T>& f) {
    if (!f.valid()) {
        return;
    }
    try {
        f.wait();
        (void)f.get();
    } catch (...) { // catch-all-ok: swallow future exception on shutdown drain
    }
}

} // namespace

void DrainUiDrawSessionFuturesBeforeAppTeardown(AppController& app) {
    (void)app;
    UiDrawSession& d = g_ui;

    // Final synchronous Save if a Preferences mutation is still dirty — catches the
    // "user changed a setting + immediately quit before the 100 ms debounce fired"
    // case. See SmatchetUiSession.h MarkPrefsDirty + SmatchetUI.cpp Draw tail.
    if (d.cfgInitialized && d.prefsDirty) {
        ConfigManager::Save(d.cfg);
        d.prefsDirty = false;
    }

    // DR8: the app-update-check worker (StartAppUpdateCheckAsync) captures AppController& by
    // reference. Its future was previously only joined by g_ui's static destructor, which runs
    // after main() returns and after AppController is gone — a use-after-free if a check is still
    // in flight at shutdown. Drain it here while AppController is still alive (teardown ordering).
    DrainFutureJoinQuiet(d.appUpdateFuture);
    d.appUpdateCheckInFlight = false;

    DrainFutureJoinQuiet(d.fieldCatalogFuture);
    d.fieldCatalogLoading = false;
    d.fieldCatalogFetchStarted = false;

    DrainFutureJoinQuiet(d.initialTicketSyncFuture);
    d.initialTicketSyncLoading = false;
    d.initialTicketSyncStarted = false;

    DrainFutureJoinQuiet(d.connectivityRecoveryTicketFetchFuture);
    d.connectivityRecoveryTicketFetchLoading = false;
    d.connectivityRecoveryTicketResyncPending = false;

    d.hasInFlightEdit = false;

    DrainFutureJoinQuiet(d.newIssueCreateFuture);
    d.newIssueCreateInFlight = false;

    DrainFutureJoinQuiet(d.quickCreateFuture);
    d.quickCreateInFlight = false;

    // WS-A: signal cancel BEFORE the join so any still-running create short-circuits
    // its remaining network/refresh work — the join then drains fast (signal → join).
    // The captured AppController is still alive here (teardown ordering), preserving
    // the no-UAF guarantee (Pillar-3).
    d.bulkImportCancel.Cancel();
    for (auto& f : d.bulkImportFutures) {
        DrainFutureJoinQuiet(f);
    }
    d.bulkImportFutures.clear();
    // Abandoned-but-still-running creates from earlier `.clear()` calls this session.
    for (auto& f : d.bulkImportFutureGraveyard) {
        DrainFutureJoinQuiet(f);
    }
    d.bulkImportFutureGraveyard.clear();
    d.bulkImportRunning = false;

    DrainAuditReloadFuture(d);
}

UiDrawSession::~UiDrawSession() { DrainAuditReloadFuture(*this); }
