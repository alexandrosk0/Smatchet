#include "SmatchetUI.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "TrackerGridFieldDisplay.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetImageTextureCache.h"
#include "SmatchetAttachmentPreviewUi.h"
#include "Logger.h"
#include "NavigationHistory.h"
#include "UiPerfMonitor.h"
#include "SmatchetPerfUi.h"
#include "SmatchetUiSession.h"
#include "Win32PickFiles.h"
#if defined(SMATCHET_WITH_MCP)
#include "SmatchetMcpServerUi.h"
#endif
#include "SmatchetToast.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <iterator>
#include <mutex>
#include <vector>
#include <string>

UiDrawSession g_ui;
static SmatchetPerfUi g_perfUi;
static bool g_openFilePathsHandlerInstalled = false;

static void ApplyLoggingSettingsFromConfig(const TrackerConfig& cfg) {
    Logger::Instance().SetMinLevel(Logger::ParseLogLevelString(cfg.LogMinLevel, LogLevel::Info));
    Logger::Instance().SetLogTrackerHttpBodies(cfg.LogTrackerHttpBodies);
    Logger::Instance().SetLogP4Io(cfg.LogP4Io);
}

struct LayoutRect {
    ImVec2 Pos;
    ImVec2 Size;
};

static float ClampFloat(float v, float lo, float hi) {
    return (std::max)(lo, (std::min)(v, hi));
}

static LayoutRect DefaultLayoutRectFor(const char* key, float defaultW, float defaultH) {
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
        const float sideW = ClampFloat(defaultW, 420.0f, (std::min)(720.0f, w * 0.46f));
        const float sideH = ClampFloat(defaultH, 420.0f, h * 0.88f);
        return LayoutRect{ImVec2(workPos.x + w - sideW - 24.0f, workPos.y + 48.0f), ImVec2(sideW, sideH)};
    }
    return centered(defaultW, defaultH);
}

static bool WindowNeedsRepair(const ImVec2& pos, const ImVec2& size, float minW, float minH) {
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

static bool IsSessionUtilityLayoutKey(const char* key) {
    return std::strcmp(key, "audit") == 0 || std::strcmp(key, "bulk_import") == 0 ||
           std::strcmp(key, "bulk_export") == 0 || std::strcmp(key, "attachment") == 0;
}

static void PersistWindowOpenPreferences(UiDrawSession& d) {
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

static std::future<FieldCatalogFetchResult> StartFieldCatalogFetchAsync(AppController& app,
                                                                        const TrackerConfig& fetchCfg) {
    return std::async(std::launch::async, [&app, fetchCfg]() {
        FieldCatalogFetchResult result;
        result.BackendKey = ConfigManager::NormalizeViewsBackendKey(fetchCfg.TrackerType);
        std::string error;
        TrackerFieldCatalogResult catalog;
        result.Ok = app.FetchFieldCatalog(fetchCfg, catalog, error);
        if (!result.Ok) {
            result.Error = error;
            return result;
        }
        result.Fields = std::move(catalog.Fields);
        result.Components = std::move(catalog.Components);
        result.IssueTypeMeta = std::move(catalog.IssueTypeMeta);
        result.Users = std::move(catalog.Users);
        result.Warning = std::move(catalog.Warning);
        return result;
    });
}

void SmatchetUI::prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                       bool requestFocus) {
    const LayoutRect rect = DefaultLayoutRectFor(layoutKey, defaultW, defaultH);
    const ImGuiCond cond = (d.layoutForceDefaultsFrames > 0)
                                ? ImGuiCond_Always
                                : (IsSessionUtilityLayoutKey(layoutKey) ? ImGuiCond_Appearing : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(rect.Pos, cond);
    ImGui::SetNextWindowSize(rect.Size, cond);
    if (requestFocus) {
        ImGui::SetNextWindowFocus();
    }
}

void SmatchetUI::repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH) {
    if (ImGui::IsWindowDocked() && d.layoutForceDefaultsFrames <= 0) {
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
    const bool keepPreferencesOpen = d.showPreferences;
    d.showPreferences = keepPreferencesOpen;
    d.showViewsDashboard = true;
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
    ConfigManager::WriteDefaultImGuiSettingsFile();
    ImGui::LoadIniSettingsFromDisk(ConfigManager::GetImGuiSettingsPath().c_str());
    PersistWindowOpenPreferences(d);
}

void SmatchetUI::Draw(AppController& app) {
    UiDrawSession& d = g_ui;
    if (!g_ui.cfgInitialized) {
        g_ui.cfg = ConfigManager::Load();
        g_ui.cfg.UiLanguage = SmatchetLocalization::NormalizeLanguageCode(g_ui.cfg.UiLanguage);
        SmatchetLocalization::SetLanguage(g_ui.cfg.UiLanguage);
        g_ui.showPreferences = g_ui.cfg.ShowPreferencesWindow;
        g_ui.showViewsDashboard = g_ui.cfg.ShowViewsDashboardWindow;
        g_ui.showPerformance = g_ui.cfg.ShowPerformanceWindow;
        g_ui.showLogWindow = g_ui.cfg.ShowLogWindow;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        g_ui.showLuaAutomationWindow = g_ui.cfg.ShowLuaAutomationWindow;
#endif
#if defined(SMATCHET_WITH_MCP)
        g_ui.showMcpServerWindow = g_ui.cfg.ShowMcpServerWindow;
#endif
        g_ui.cfgInitialized = true;
        ApplyLoggingSettingsFromConfig(g_ui.cfg);

        if (!g_ui.cfg.SelectedFontName.empty() && g_ui.cfg.SelectedFontName != "Segoe UI") {
            SmatchetRequestFontReload(g_ui.cfg.SelectedFontName, 16.0f);
        }

        app.SetAiPromptHandler([](const std::string& message) {
            g_ui.aiPromptPending = true;
            g_ui.aiPromptMessage = message;
        });
    }
    if (d.cfgInitialized && !d.offlineLegacyStartupBannerConsumed) {
        d.offlineLegacyStartupBannerConsumed = true;
        d.offlineLegacyStartupBannerText = app.TakeLegacyPendingStartupBanner();
    }
    if (d.deadLetterPanelStatusHasClearDeadline && std::chrono::steady_clock::now() >= d.deadLetterPanelStatusClearAt) {
        d.deadLetterPanelStatus.clear();
        d.deadLetterPanelStatusHasClearDeadline = false;
    }
    if (d.offlineQueuePanelStatusHasClearDeadline &&
        std::chrono::steady_clock::now() >= d.offlineQueuePanelStatusClearAt) {
        d.offlineQueuePanelStatus.clear();
        d.offlineQueuePanelStatusHasClearDeadline = false;
    }
    // Drain the offline create queue opportunistically (rate-limited internally).
    app.TickOfflineCreates();
    app.TickOfflineFieldEdits();
    app.TickStreamingApply();
    if (!g_ui.attachmentPreviewCallbackRegistered) {
        app.SetAttachmentPreviewHandler([](const std::string& localPath, const std::string& mimeType,
                                           const std::string& filename, const std::string& sourceUrl) -> bool {
            if (!IsSupportedImageMime(mimeType)) {
                return false;
            }
            std::lock_guard<std::mutex> lock(g_ui.attachmentPreviewMutex);
            AttachmentPreviewUpdate update;
            update.LocalPath = localPath;
            update.MimeType = mimeType;
            update.Filename = filename;
            update.Url = sourceUrl;
            g_ui.attachmentPreviewUpdateQueue.push_back(std::move(update));
            return true;
        });
        app.SetAttachmentCollectionHandler([](const std::vector<AppController::AttachmentDescriptor>& attachments) {
            std::lock_guard<std::mutex> lock(g_ui.attachmentPreviewMutex);
            AttachmentCollectionRequest request;
            request.Attachments = attachments;
            g_ui.attachmentCollectionQueue.push_back(std::move(request));
        });
        g_ui.attachmentPreviewCallbackRegistered = true;
    }
#if defined(_WIN32)
    if (!g_openFilePathsHandlerInstalled) {
        app.SetOpenFilePathsHandler([](bool allowMultiple, const std::string& initialDirectoryUtf8,
                                       std::function<void(std::vector<std::string>)> onComplete) {
            // GLFW: PlatformHandle is GLFWwindow*; PlatformHandleRaw is HWND (see imgui_impl_glfw).
            void* hwnd = nullptr;
            if (ImGui::GetCurrentContext()) {
                if (const ImGuiViewport* vp = ImGui::GetMainViewport()) {
                    hwnd = vp->PlatformHandleRaw;
                }
            }
            std::vector<std::string> paths;
            std::string lastDir;
            if (SmatchetWin32PickOpenFilePaths(hwnd, allowMultiple, initialDirectoryUtf8, &lastDir, paths)) {
                if (!lastDir.empty()) {
                    g_ui.cfg.LastImportDirectory = std::move(lastDir);
                    ConfigManager::Save(g_ui.cfg);
                }
            }
            if (onComplete) {
                onComplete(std::move(paths));
            }
        });
        g_openFilePathsHandlerInstalled = true;
    }
#endif
    UiPerfMonitor::Instance().BeginFrame();
    SmatchetImageTextureCache::TickPendingDestroys();
    SMATCHET_UI_PERF_SCOPE("SmatchetUI::Draw");
    {
        SMATCHET_UI_PERF_SCOPE("ViewState::EnsureLoaded");
        ViewState.EnsureLoaded(g_ui.cfg);
        const std::string bk = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        if (!d.lastViewsBackendKey.empty() && d.lastViewsBackendKey != bk) {
            d.appliedInitialView = false;
            d.initialTicketSyncStarted = false;
            d.initialTicketSyncLoading = false;
            d.initialTicketSyncFuture = {};
            d.connectivityRecoveryTicketFetchLoading = false;
            d.connectivityRecoveryTicketFetchFuture = {};
            d.connectivityRecoveryTicketResyncPending = false;
            d.triggerCatalogRefetch = true;
            d.editingViewId.clear();
        }
        d.lastViewsBackendKey = bk;
    }
    {
        SMATCHET_UI_PERF_SCOPE("TickTrackerConnectivityMonitor");
        app.TickTrackerConnectivityMonitor(g_ui.cfg);
    }
    auto clearStaleQueuedOfflineGridBanner = []() {
        // Stale green banner: "Queued offline…" is wrong once Jira is reachable again (probe or live API).
        if (!g_ui.gridEditSuccess.empty() && g_ui.gridEditSuccess.find("Queued offline") != std::string::npos) {
            g_ui.gridEditSuccess.clear();
        }
    };
    if (app.ConsumeTrackerConnectivityRecovery()) {
        g_ui.triggerCatalogRefetch = true;
        // A live ticket sync already refreshes the grid; queueing resync here while streaming runs
        // caused a second "Syncing" toast after the first session finished.
        if (!app.IsStreamingSyncActive()) {
            g_ui.connectivityRecoveryTicketResyncPending = true;
        }
        clearStaleQueuedOfflineGridBanner();
    }
    if (app.ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny()) {
        clearStaleQueuedOfflineGridBanner();
    }
    if (app.ConsumeFieldCatalogRefetchAfterLiveTicketSync()) {
        g_ui.triggerCatalogRefetch = true;
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawEnsureCatalogAndInitialSync");
        drawEnsureCatalogAndInitialSync(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawMainMenuBar");
        drawMainMenuBar(app, d);
    }
    if (!d.offlineLegacyStartupBannerText.empty()) {
        SMATCHET_UI_PERF_SCOPE("drawLegacyStartupBanner");
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", d.offlineLegacyStartupBannerText.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Dismiss##legacyStartupBanner")) {
            d.offlineLegacyStartupBannerText.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hide this one-time migration notice.");
        }
        ImGui::Separator();
    }
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetPerfUi::DrawWindow");
        if (g_ui.showPerformance) {
            prepareTopLevelWindow(g_ui, "performance", 580.0f, 380.0f);
        }
        g_perfUi.DrawWindow(&g_ui.showPerformance);
    }
    blameAnalysisUi_.SetBlamePanelOpen(g_ui.showBlameAnalysis);
    blameAnalysisUi_.ServiceBackground();
    if (g_ui.showBlameAnalysis) {
        blameAnalysisUi_.DrawWindow(app, &g_ui.showBlameAnalysis, g_ui.gridState.ActiveIssueId);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawPreferencesWindow");
        drawPreferencesWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawViewsDashboardWindow");
        drawViewsDashboardWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawActiveProjectWindow");
        drawActiveProjectWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawAttachmentPreviewWindow");
        drawAttachmentPreviewWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawBulkImportWindow");
        drawBulkImportWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawBulkExportWindow");
        drawBulkExportWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetToastManager::Render");
        SmatchetToastManager::Instance().Render();
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawAuditWindow");
        drawAuditWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("TrackerGridFieldDisplay::DrawWatchersListWindow");
        TrackerGridFieldDisplay::DrawWatchersListWindow(g_ui.trackerGridAsync);
    }
    {
        SMATCHET_UI_PERF_SCOPE("TrackerGridFieldDisplay::DrawVotesListWindow");
        TrackerGridFieldDisplay::DrawVotesListWindow(g_ui.trackerGridAsync);
    }
#if defined(SMATCHET_WITH_AI)
    {
        SMATCHET_UI_PERF_SCOPE("drawAIAssistantWindow");
        drawAIAssistantWindow(app, d);
    }
#endif
#if defined(SMATCHET_WITH_MCP)
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetDrawMcpServerWindow");
        SmatchetDrawMcpServerWindow(app, d);
    }
#endif
    if (d.showLogWindow) {
        SMATCHET_UI_PERF_SCOPE("drawLogWindow");
        drawLogWindow(d);
    }
    if (g_ui.showPerformance) {
        g_perfUi.DrawFpsOverlay();
    }
    if (g_ui.pendingViewStateSave && std::chrono::steady_clock::now() >= g_ui.pendingViewStateSaveAt) {
        SMATCHET_UI_PERF_SCOPE("ViewState::SaveDebounced");
        ViewState.Save();
        g_ui.pendingViewStateSave = false;
    }
    PersistWindowOpenPreferences(g_ui);
    if (g_ui.layoutForceDefaultsFrames > 0) {
        --g_ui.layoutForceDefaultsFrames;
        if (g_ui.layoutForceDefaultsFrames == 0) {
            ImGui::SaveIniSettingsToDisk(ConfigManager::GetImGuiSettingsPath().c_str());
        }
    }
}

void SmatchetUI::drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d) {
    const auto startCatalogFetch = [&](const TrackerConfig& fetchCfg) {
        if (d.fieldCatalogLoading) {
            return;
        }
        d.fieldCatalogLoading = true;
        d.fieldCatalogFetchStarted = true;
        d.fieldCatalogFuture = StartFieldCatalogFetchAsync(app, fetchCfg);
    };

    if ((!d.fieldCatalogFetchStarted || d.triggerCatalogRefetch) && !d.fieldCatalogLoading) {
        d.triggerCatalogRefetch = false;
        startCatalogFetch(d.cfg);
    }

    if (d.fieldCatalogLoading && d.fieldCatalogFuture.valid() &&
        d.fieldCatalogFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
            FieldCatalogFetchResult result = d.fieldCatalogFuture.get();
            if (result.BackendKey != ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType)) {
                d.fieldCatalogLoading = false;
                d.triggerCatalogRefetch = true;
                return;
            }
            if (result.Ok) {
                app.SetFieldCatalog(std::move(result.Fields), std::move(result.Components),
                                    std::move(result.IssueTypeMeta), std::string());
                d.fieldCatalogWarning = result.Warning;
                if (!d.fieldCatalogWarning.empty()) {
                    LOG_WARN("SmatchetUI: users fetch warning: %s", d.fieldCatalogWarning.c_str());
                }
            } else {
                app.SetFieldCatalog(
                    {}, {}, result.Error.empty() ? std::string("Failed to fetch field catalog.") : result.Error);
                d.fieldCatalogWarning.clear();
            }
        } catch (const std::exception& ex) {
            app.SetFieldCatalog({}, {}, std::string("Field catalog load failed: ") + ex.what());
            d.fieldCatalogWarning.clear();
            LOG_ERROR("SmatchetUI: field catalog future exception: %s", ex.what());
        } catch (...) {
            app.SetFieldCatalog({}, {}, "Field catalog load failed.");
            d.fieldCatalogWarning.clear();
            LOG_ERROR("SmatchetUI: field catalog future unknown exception");
        }
        d.fieldCatalogLoading = false;
    }

    if (!d.appliedInitialView) {
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            d.cfg.JqlQuery = activeView->Jql;
            d.cfg.SelectedFields = activeView->Fields;
            d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
        }
        if (!d.initialTicketSyncStarted) {
            ConfigManager::Save(d.cfg);
            app.ClearLastTrackerTicketSyncWarning();
            d.initialTicketSyncStarted = true;
            // Initial refresh already covers a same-frame connectivity-recovery latch; skip the
            // follow-up resync on the next frame (would duplicate SyncWithBackend / toasts).
            d.connectivityRecoveryTicketResyncPending = false;
            app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
            d.appliedInitialView = true;
        }
    }

    else if (d.connectivityRecoveryTicketResyncPending) {
        if (d.appliedInitialView) {
            // Avoid overlapping with an in-flight streaming ticket sync (e.g. initial load): a second
            // SyncWithBackend defers via supersede and replays with a second "Syncing" toast.
            if (app.IsStreamingSyncActive()) {
                return;
            }
            ConfigManager::Save(d.cfg);
            app.ClearLastTrackerTicketSyncWarning();
            d.connectivityRecoveryTicketResyncPending = false;
            app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
        }
    }
}

void SmatchetUI::drawMainMenuBar(AppController& app, UiDrawSession& d) {
    (void)app;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Preferences...")) {
                d.showPreferences = true;
            }
            if (ImGui::MenuItem("Performance...")) {
                d.showPerformance = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            if (ImGui::MenuItem("Open Views...")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            if (ImGui::MenuItem("Reset Window Layout")) {
                resetWindowLayoutToDefault(d);
            }
            if (ImGui::MenuItem("Blame Analysis...")) {
                d.showBlameAnalysis = true;
            }
            ImGui::Separator();
            ImGui::MenuItem("Show Log", nullptr, &d.showLogWindow);
            ImGui::Separator();
            if (ImGui::MenuItem("Backend audit...")) {
                d.showAuditTrail = true;
                d.requestAuditTrailFocus = true;
            }
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            ImGui::Separator();
            if (ImGui::MenuItem("Scripting...", nullptr, &d.showLuaAutomationWindow) && d.showLuaAutomationWindow) {
                d.requestLuaAutomationFocus = true;
                d.requestScriptingEditorTabFocus = true;
            }
#if defined(SMATCHET_WITH_MCP)
            if (ImGui::MenuItem("MCP Server...", nullptr, &d.showMcpServerWindow) && d.showMcpServerWindow) {
                d.requestMcpServerFocus = true;
            }
#endif
#endif
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Bulk")) {
            if (ImGui::MenuItem("Bulk import...")) {
                d.showBulkImport = true;
            }
            if (ImGui::MenuItem("Bulk export...")) {
                d.showBulkExport = true;
            }
            ImGui::EndMenu();
        }
#if !defined(SMATCHET_WITH_LUA_AUTOMATION)
        {
            static bool s_loggedLuaMenuAbsent = false;
            if (!s_loggedLuaMenuAbsent) {
                s_loggedLuaMenuAbsent = true;
                LOG_WARN("SmatchetUI: Lua automation disabled in this binary (no Scripting window).");
            }
        }
#endif
#if defined(SMATCHET_WITH_MCP) && !defined(SMATCHET_WITH_LUA_AUTOMATION)
        if (ImGui::BeginMenu("MCP")) {
            if (ImGui::MenuItem("MCP Server...", nullptr, &d.showMcpServerWindow) && d.showMcpServerWindow) {
                d.requestMcpServerFocus = true;
            }
            ImGui::EndMenu();
        }
#endif
#ifdef SMATCHET_EMBEDDED_IN_UNREAL
        {
            const char* closeLabel = "Close";
            const float btnW = ImGui::CalcTextSize(closeLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            constexpr float kRightMargin = 10.0f;
            const float xPos =
                (std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - btnW - kRightMargin);
            ImGui::SetCursorPosX(xPos);
            if (ImGui::SmallButton(closeLabel)) {
                app.CloseEmbeddedUi();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hide Smatchet overlay (same as Ctrl+Shift+J)");
            }
        }
#endif
        ImGui::EndMainMenuBar();
    }
}

namespace {

template <typename T>
void DrainFutureJoinQuiet(std::future<T>& f) {
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

UiDrawSession::~UiDrawSession() {
    DrainAuditReloadFuture(*this);
}
