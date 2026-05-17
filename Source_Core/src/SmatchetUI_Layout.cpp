#include "SmatchetUI_Internal.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetToast.h"

#include <algorithm>
#include <cstring>
#include <future>

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

void SmatchetUI::prepareTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                       bool requestFocus) {
    // With docking enabled, position is managed by the dock layout (imgui.ini DockId entries).
    // SetNextWindowPos with ImGuiCond_Appearing or ImGuiCond_Always fights the dock engine:
    // it forces windows to absolute pixel coordinates, kicking them out of their dock nodes
    // on every toggle. Only set size on FirstUseEver as a fallback for windows with no ini entry.
    ImGui::SetNextWindowSize(ImVec2(defaultW, defaultH), ImGuiCond_FirstUseEver);
    if (requestFocus) {
        ImGui::SetNextWindowFocus();
    }
    (void)layoutKey; // previously used for position lookup — no longer needed with docking
    (void)d;
}

void SmatchetUI::repairTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float minW, float minH) {
    // Docked windows: size and position are fully managed by the dock node. Do not repair.
    // (Used to special-case the Smatchet Assistant side panel here because it ran as a
    // floating non-docked window with ImGuiCond_Always SetNextWindowPos; that layout was
    // replaced with dock-system integration in 2026-05-17. The IsWindowDocked guard below
    // already covers the new path.)
    if (ImGui::IsWindowDocked()) {
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
    d.showViewsDashboard = true;
    d.requestActiveProjectFocus = false;
    d.requestViewsDashboardFocus = false;
    d.showPerformance = false;
    d.showBlameAnalysis = false;
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
    d.layoutForceDefaultsFrames = 8;
    // Reset the on-disk ini so the next launch picks up the default dock layout.
    // ImGui::LoadIniSettingsFromDisk() at runtime does NOT re-parent already-created
    // windows: docking metadata gets replaced, but live windows lose their dock parents
    // and float free. Skip the runtime reload and ask the user to restart.
    ConfigManager::WriteDefaultImGuiSettingsFile();
    SmatchetToastManager::Instance().Push("Layout reset", "Restart Smatchet for the default layout to take effect.",
                                          ToastType::Info, 4000);
    smatchet::ui_detail::PersistWindowOpenPreferences(d);
}

void SmatchetUI_ResetLayoutToDefault(UiDrawSession& d) {
    // Replicates SmatchetUI::resetWindowLayoutToDefault for callers without a SmatchetUI*.
    // Must be called on the UI thread (see MainThreadDispatch.h).
    d.showViewsDashboard = true;
    d.requestActiveProjectFocus = false;
    d.requestViewsDashboardFocus = false;
    d.showPerformance = false;
    d.showBlameAnalysis = false;
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
    d.layoutForceDefaultsFrames = 8;
    // Reset the on-disk ini so the next launch picks up the default dock layout.
    // ImGui::LoadIniSettingsFromDisk() at runtime does NOT re-parent already-created
    // windows: docking metadata gets replaced, but live windows lose their dock parents
    // and float free. Skip the runtime reload and ask the user to restart.
    ConfigManager::WriteDefaultImGuiSettingsFile();
    SmatchetToastManager::Instance().Push("Layout reset", "Restart Smatchet for the default layout to take effect.",
                                          ToastType::Info, 4000);
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
    } catch (...) {
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

    for (auto& f : d.bulkImportFutures) {
        DrainFutureJoinQuiet(f);
    }
    d.bulkImportFutures.clear();
    d.bulkImportRunning = false;

    DrainAuditReloadFuture(d);
}

UiDrawSession::~UiDrawSession() { DrainAuditReloadFuture(*this); }
