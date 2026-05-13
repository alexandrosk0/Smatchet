#include "SmatchetUI.h"
#include "AppController.h"
#include "SmatchetViewVisibility.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetStatusBarUi.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/ViewCommands.h"
#include "ConfigManager.h"
#include "TrackerGridFieldDisplay.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetImageTextureCache.h"
#include "SmatchetAttachmentPreviewUi.h"
#include "SmatchetTheme.h"
#include "Logger.h"
#include "NavigationHistory.h"
#include "ProjectResolver.h"
#include "TicketGridModel.h"
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
#include <string>
#include <vector>

UiDrawSession g_ui;
static SmatchetPerfUi g_perfUi;
static bool g_openFilePathsHandlerInstalled = false;

static void ApplyLoggingSettingsFromConfig(const TrackerConfig& cfg) {
    Logger::Instance().SetMinLevel(Logger::ParseLogLevelString(cfg.LogMinLevel, LogLevel::Info));
    Logger::Instance().SetLogTrackerHttpBodies(cfg.LogTrackerHttpBodies);
    Logger::Instance().SetLogP4Io(cfg.LogP4Io);
}

static std::future<AppUpdateInfo> StartAppUpdateCheckAsync(AppController& app, const TrackerConfig& cfg) {
    const bool includePrerelease = cfg.UpdateIncludePrerelease;
    return std::async(std::launch::async,
                      [&app, includePrerelease]() { return app.CheckForAppUpdate(includePrerelease); });
}

static void StartAppUpdateCheck(UiDrawSession& d, AppController& app, bool manual) {
    if (d.appUpdateCheckInFlight) {
        return;
    }
    d.appUpdateActionStatus.clear();
    d.appUpdateCheckManual = manual;
    d.appUpdateCheckInFlight = true;
    d.appUpdateFuture = StartAppUpdateCheckAsync(app, d.cfg);
}

static void DrainAppUpdateCheck(UiDrawSession& d) {
    if (!d.appUpdateCheckInFlight || !d.appUpdateFuture.valid()) {
        return;
    }
    if (d.appUpdateFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    d.appUpdateCheckInFlight = false;
    try {
        d.appUpdateInfo = d.appUpdateFuture.get();
    } catch (const std::exception& ex) {
        d.appUpdateInfo = {};
        d.appUpdateInfo.Error = std::string("Update check failed: ") + ex.what();
    } catch (...) {
        d.appUpdateInfo = {};
        d.appUpdateInfo.Error = "Update check failed with an unknown error.";
    }

    if (!d.appUpdateInfo.Error.empty()) {
        if (d.appUpdateCheckManual) {
            SmatchetToastManager::Instance().Push("Updates", d.appUpdateInfo.Error, ToastType::Error, 5000);
        }
        return;
    }
    if (!d.appUpdateInfo.UpdateAvailable) {
        if (d.appUpdateCheckManual) {
            SmatchetToastManager::Instance().Push("Updates", "You are already on the latest release.",
                                                  ToastType::Success, 3500);
        }
        return;
    }
    if (!d.appUpdateCheckManual && !d.cfg.UpdateSkipVersion.empty() &&
        d.cfg.UpdateSkipVersion == d.appUpdateInfo.LatestVersion) {
        return;
    }

    d.appUpdateModalOpen = true;
    ImGui::OpenPopup("Update Available");
    if (d.appUpdateCheckManual) {
        SmatchetToastManager::Instance().Push("Updates", "New version found.", ToastType::Info, 2500);
    }
}

static void DrawAppUpdateModal(AppController& app, UiDrawSession& d) {
    if (d.appUpdateModalOpen) {
        ImGui::OpenPopup("Update Available");
    }
    if (!ImGui::BeginPopupModal("Update Available", &d.appUpdateModalOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Current version: %s", d.appUpdateInfo.CurrentVersion.c_str());
    ImGui::Text("Latest version:  %s", d.appUpdateInfo.LatestVersion.c_str());
    if (!d.appUpdateInfo.ReleaseTag.empty()) {
        ImGui::TextDisabled("Release tag: %s", d.appUpdateInfo.ReleaseTag.c_str());
    }
    if (!d.appUpdateInfo.ReleaseUrl.empty() && ImGui::SmallButton("Open Release Page")) {
        app.OpenUrl(d.appUpdateInfo.ReleaseUrl);
    }

    ImGui::Spacing();
    ImGui::TextWrapped("A newer Smatchet standalone release is available on GitHub.");
    if (!d.appUpdateInfo.ReleaseNotes.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Release notes");
        ImGui::BeginChild("UpdateReleaseNotes", ImVec2(620.0f, 220.0f), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", d.appUpdateInfo.ReleaseNotes.c_str());
        ImGui::EndChild();
    }
    if (!d.appUpdateActionStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", d.appUpdateActionStatus.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button("Download and Install", ImVec2(170.0f, 0.0f))) {
        std::string err;
        if (app.DownloadAndLaunchInstallerUpdate(d.appUpdateInfo.InstallerAsset.DownloadUrl,
                                                 d.appUpdateInfo.InstallerAsset.Name, err)) {
            d.appUpdateActionStatus = "Installer launched. Smatchet will close so the update can proceed.";
        } else {
            d.appUpdateActionStatus = err.empty() ? "Failed to launch installer update." : err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip This Version", ImVec2(150.0f, 0.0f))) {
        d.cfg.UpdateSkipVersion = d.appUpdateInfo.LatestVersion;
        ConfigManager::Save(d.cfg);
        d.appUpdateModalOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later", ImVec2(90.0f, 0.0f))) {
        d.appUpdateModalOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

struct LayoutRect {
    ImVec2 Pos;
    ImVec2 Size;
};

static float ClampFloat(float v, float lo, float hi) { return (std::max)(lo, (std::min)(v, hi)); }

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
        const float utilityPanelW = ClampFloat(defaultW, 420.0f, (std::min)(720.0f, w * 0.46f));
        const float sideH = ClampFloat(defaultH, 420.0f, h * 0.88f);
        return LayoutRect{ImVec2(workPos.x + w - utilityPanelW - 24.0f, workPos.y + 48.0f),
                          ImVec2(utilityPanelW, sideH)};
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
                                                                        const TrackerConfig& fetchCfg,
                                                                        const std::string& projectKey) {
    return std::async(std::launch::async, [&app, fetchCfg, projectKey]() {
        FieldCatalogFetchResult result;
        result.BackendKey = ConfigManager::NormalizeViewsBackendKey(fetchCfg.TrackerType);
        std::string error;
        TrackerFieldCatalogResult catalog;
        // Pass active-view projectKey so Jira createmeta + /status enrichment populates
        // priority/status AllowedValueOptions (otherwise grid renders those cells as text).
        result.Ok = app.FetchFieldCatalog(fetchCfg, projectKey, catalog, error);
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
    ConfigManager::WriteDefaultImGuiSettingsFile();
    ImGui::LoadIniSettingsFromDisk(ConfigManager::GetImGuiSettingsPath().c_str());
    PersistWindowOpenPreferences(d);
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
        // Commit-last layout migration: reset dock layout first, then bump version and
        // persist. If killed between the two steps the next launch re-migrates safely.
        // ConfigManager::Save uses AtomicWriteTextFile (write-tmp + MoveFileEx rename)
        // internally via WriteConfigJson — already atomic.
        if (g_ui.cfg.LayoutSchemaVersion < ConfigManager::kCurrentLayoutSchemaVersion) {
            LOG_INFO("SmatchetUI: LayoutSchemaVersion %d < %d — resetting layout to VS shell default.",
                     g_ui.cfg.LayoutSchemaVersion, ConfigManager::kCurrentLayoutSchemaVersion);
            resetWindowLayoutToDefault(g_ui);
            g_ui.cfg.LayoutSchemaVersion = ConfigManager::kCurrentLayoutSchemaVersion;
            ConfigManager::Save(g_ui.cfg);
        }

        g_ui.cfgInitialized = true;
        ApplyLoggingSettingsFromConfig(g_ui.cfg);

        if (!g_ui.cfg.SelectedFontName.empty() && g_ui.cfg.SelectedFontName != "Segoe UI") {
            SmatchetRequestFontReload(g_ui.cfg.SelectedFontName, 16.0f);
        }
    }

    // Zoom: per-frame FontGlobalScale from cfg.FontSizePt. Cheap, instant, no atlas rebuild.
    ::ImGui::GetIO().FontGlobalScale = static_cast<float>(d.cfg.FontSizePt) / 16.0f;

    // Apply density padding — only re-apply when the setting changes.
    {
        if (d.cfg.Density != lastAppliedDensity_) {
            lastAppliedDensity_ = d.cfg.Density;
            ImGuiStyle& style = ::ImGui::GetStyle();
            switch (d.cfg.Density) {
                case TrackerConfig::UiDensity::Compact:
                    style.ItemSpacing  = ImVec2(4.0f, 2.0f);
                    style.FramePadding = ImVec2(4.0f, 2.0f);
                    break;
                case TrackerConfig::UiDensity::Comfortable:
                    style.ItemSpacing  = ImVec2(10.0f, 8.0f);
                    style.FramePadding = ImVec2(8.0f, 6.0f);
                    break;
                default: // Normal
                    style.ItemSpacing  = ImVec2(8.0f, 6.0f);
                    style.FramePadding = ImVec2(6.0f, 4.0f);
                    break;
            }
        }
    }

    // Panel visibility is driven by the d.show* / cfg.Show* flags that gate each ImGui::Begin call.
    // ImGui collapses empty dock nodes automatically — no per-frame bit-manipulation needed.
    // (HiddenTabBar fights the layout on resize; removed in favour of the natural empty-node path.)

    // Re-apply the style palette only when cfg.Theme drifts from what is live in ImGui::GetStyle().
    // SmatchetImGuiHost seeds SmatchetDark before cfg is loaded; the first frame after Load() catches
    // any user-saved value through this check.
    if (d.cfg.Theme != lastAppliedTheme_) {
        SmatchetTheme::ApplyStyle(d.cfg.Theme);
        lastAppliedTheme_ = d.cfg.Theme;
    }

    if (d.cfgInitialized && !d.offlineLegacyStartupBannerConsumed) {
        d.offlineLegacyStartupBannerConsumed = true;
        d.offlineLegacyStartupBannerText = app.TakeLegacyPendingStartupBanner();
    }
    if (d.cfgInitialized && d.cfg.UpdateCheckEnabled && !d.appUpdateStartupCheckStarted) {
        d.appUpdateStartupCheckStarted = true;
        StartAppUpdateCheck(d, app, false);
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
    d.cachedPendingFieldEditCount = static_cast<int>(app.GetPendingFieldEdits().size());
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

    // Unified Command System: palette (Ctrl+Shift+P) — must run before any
    // sub-window draws so it can intercept key events first.
    commandPalette_.Draw(app);

    // Scenario tick: drive the active scenario one frame and propagate scroll state
    // into the session so SmatchetActiveProjectGridUi can honor it.
    {
        bool scenScrollActive = false;
        int scenScrollTarget = -1;
        app.Scenarios().Tick(app, scenScrollActive, scenScrollTarget);
        d.scenarioScrollActive = scenScrollActive;
        d.scenarioScrollTarget = scenScrollTarget;
    }

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
    // Register view.* commands once ViewState is loaded (idempotent — skips on 2nd+ call).
    smatchet::cmd::RegisterViewCommands(app, ViewState);
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
        SMATCHET_UI_PERF_SCOPE("MainThreadDispatcher::Drain");
        app.mainThreadDispatcher.Drain();
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawEnsureCatalogAndInitialSync");
        drawEnsureCatalogAndInitialSync(app, d);
    }
    {
        // Build the per-frame catalog+column cache once before any consumer.
        const std::uint64_t catalogRev = app.GetFieldCatalogRevision();
        const std::uint64_t viewsRev = ViewState.GetRevision();
        ViewDefinition* av = ViewState.GetActiveViewMutable();
        const std::string avId = av ? av->Id : "";
        // viewsRevision invalidates the cache when the active view is mutated
        // in place (column widths, fields, order edits) without its id changing.
        if (!gridFrameCtx_.catalogIndex || gridFrameCtx_.catalogRevision != catalogRev ||
            gridFrameCtx_.viewsRevision != viewsRev || gridFrameCtx_.activeViewId != avId) {
            gridFrameCtx_.catalogRevision = catalogRev;
            gridFrameCtx_.viewsRevision = viewsRev;
            gridFrameCtx_.activeViewId = avId;
            gridFrameCtx_.catalogIndex = std::make_unique<TrackerFieldCatalogIndex>(app.GetAvailableFields());
            gridFrameCtx_.columns = av ? TicketGridColumnsBuilder::Build(*av, *gridFrameCtx_.catalogIndex)
                                       : std::vector<TicketGridColumn>();
        }
    }
    if (!d.cfg.ZenMode) {
        SMATCHET_UI_PERF_SCOPE("drawMainMenuBar");
        drawMainMenuBar(app, d);
    }
    // Ctrl+Alt+D — toggle dock-node debug overlay.
    {
        const ImGuiIO& dbgIo = ::ImGui::GetIO();
        if (dbgIo.KeyCtrl && dbgIo.KeyAlt && ::ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            d.showDockDebug = !d.showDockDebug;
        }
    }
    // Status bar — must be drawn before dockspace/other windows (viewport side-bar reservation).
    if (d.cfg.ShowStatusBar && !d.cfg.ZenMode) {
        DrawStatusBar(app, d);
    }

    // F11 — full screen toggle (standalone only).
#ifndef SMATCHET_EMBEDDED_IN_UNREAL
    if (::ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        d.requestFullScreenToggle = true;
    }
#endif

    // Zen Mode: Ctrl+M then Z chord (1 s timeout). Esc Esc to exit.
    {
        struct KeyChord {
            bool prefixArmed  = false;
            float timeoutSec  = 0.0f;

            bool Tick(bool prefixKey, bool completionKey, float dt) {
                static const float kTimeout = 1.0f;
                if (prefixKey) {
                    prefixArmed = true;
                    timeoutSec  = 0.0f;
                }
                if (prefixArmed) {
                    timeoutSec += dt;
                    if (timeoutSec > kTimeout) {
                        prefixArmed = false;
                    }
                    if (::ImGui::GetIO().WantTextInput) {
                        prefixArmed = false;
                    }
                    if (completionKey && prefixArmed) {
                        prefixArmed = false;
                        return true;
                    }
                }
                return false;
            }
        };
        static KeyChord s_zenChord;

        const ImGuiIO& zcIo  = ::ImGui::GetIO();
        const bool ctrlM = zcIo.KeyCtrl && !zcIo.KeyShift && !zcIo.KeyAlt
                           && ::ImGui::IsKeyPressed(ImGuiKey_M, false);
        const bool keyZ  = !zcIo.KeyCtrl && !zcIo.KeyShift && !zcIo.KeyAlt
                           && ::ImGui::IsKeyPressed(ImGuiKey_Z, false);
        if (s_zenChord.Tick(ctrlM, keyZ, zcIo.DeltaTime)) {
            d.cfg.ZenMode = !d.cfg.ZenMode;
        }
    }
    // Esc Esc to exit Zen Mode.
    if (d.cfg.ZenMode) {
        static int s_escCount   = 0;
        static float s_escTimer = 0.0f;
        s_escTimer += ::ImGui::GetIO().DeltaTime;
        if (s_escTimer > 0.5f) {
            s_escCount = 0;
            s_escTimer = 0.0f;
        }
        if (::ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ++s_escCount;
            s_escTimer = 0.0f;
            if (s_escCount >= 2) {
                d.cfg.ZenMode = false;
                s_escCount    = 0;
            }
        }
    }

    // Keyboard shortcuts for panel visibility toggles (Ctrl+B / Ctrl+J).
    // ImGui::Shortcut available in docking branch; fall back to GetIO check if absent.
    {
        const ImGuiIO& io = ::ImGui::GetIO();
        const bool ctrlDown = io.KeyCtrl;
        const bool altDown = io.KeyAlt;
        if (ctrlDown && !altDown && ::ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            SetViewVisible(d.cfg, ViewSlot::PrimarySideBar, !d.cfg.ShowPrimarySideBar);
            ConfigManager::Save(d.cfg);
        }
        if (ctrlDown && altDown && ::ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            SetViewVisible(d.cfg, ViewSlot::SecondarySideBar, !d.cfg.ShowSecondarySideBar);
            ConfigManager::Save(d.cfg);
        }
        if (ctrlDown && !altDown && ::ImGui::IsKeyPressed(ImGuiKey_J, false)) {
            SetViewVisible(d.cfg, ViewSlot::BottomPanel, !d.cfg.ShowPanel);
            ConfigManager::Save(d.cfg);
        }
    }
    DrainAppUpdateCheck(d);
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
    DrawAppUpdateModal(app, d);
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
    // Dock-node debug overlay — toggled by Ctrl+Alt+D.
    if (d.showDockDebug) {
        const ImGuiWindowFlags kDbgFlags = ImGuiWindowFlags_NoDocking
                                         | ImGuiWindowFlags_NoCollapse
                                         | ImGuiWindowFlags_AlwaysAutoResize
                                         | ImGuiWindowFlags_NoSavedSettings;
        ::ImGui::Begin("##DockDebug", nullptr, kDbgFlags);
        ::ImGui::TextDisabled("Dock Node Debug (Ctrl+Alt+D to hide)");
        ::ImGui::Separator();
        static const ImGuiID kNodes[] = {
            SmatchetDockNodeIds::kPrimarySideBar,
            SmatchetDockNodeIds::kBottomPanel,
            SmatchetDockNodeIds::kSecondarySideBar,
        };
        static const char* const kNames[] = {
            "PrimarySideBar(0x4)",
            "BottomPanel(0xA)",
            "SecondarySideBar(0x10)",
        };
        for (int i = 0; i < 3; ++i) {
            ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(kNodes[i]);
            if (!node) {
                ::ImGui::TextDisabled("%s: NOT FOUND", kNames[i]);
                continue;
            }
            ::ImGui::Text("%s: size=(%.0f,%.0f) flags=0x%X tabs=%d empty=%d",
                kNames[i],
                node->Size.x, node->Size.y,
                static_cast<int>(node->LocalFlags),
                node->Windows.Size,
                static_cast<int>(node->IsEmpty()));
        }
        ::ImGui::Separator();
        ::ImGui::Text("ShowPrimary=%d ShowPanel=%d ShowSecondary=%d",
            d.cfg.ShowPrimarySideBar, d.cfg.ShowPanel, d.cfg.ShowSecondarySideBar);
        ::ImGui::End();

        // Per-frame LOG_DEBUG throttled to every 120 frames.
        {
            static int s_dbgLogFrame = 0;
            ++s_dbgLogFrame;
            if (s_dbgLogFrame >= 120) {
                s_dbgLogFrame = 0;
                for (int i = 0; i < 3; ++i) {
                    ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(kNodes[i]);
                    if (!node) {
                        LOG_DEBUG("DockDebug: %s NOT FOUND", kNames[i]);
                    } else {
                        LOG_DEBUG("DockDebug: %s size=(%.0f,%.0f) flags=0x%X tabs=%d empty=%d",
                            kNames[i],
                            node->Size.x, node->Size.y,
                            static_cast<int>(node->LocalFlags),
                            node->Windows.Size,
                            static_cast<int>(node->IsEmpty()));
                    }
                }
                LOG_DEBUG("DockDebug: ShowPrimary=%d ShowPanel=%d ShowSecondary=%d",
                    d.cfg.ShowPrimarySideBar, d.cfg.ShowPanel, d.cfg.ShowSecondarySideBar);
            }
        }
    }
    // Skip the debounced auto-save while a view edit is pending an explicit Save —
    // widths / sort specs mutated under the unsaved-layout strip must not bleed
    // through to disk until the user commits.
    if (g_ui.pendingViewStateSave && !g_ui.viewsDirty &&
        std::chrono::steady_clock::now() >= g_ui.pendingViewStateSaveAt) {
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
        const ViewDefinition* activeView = ViewState.GetActiveView();
        const std::string jql = activeView ? activeView->Jql : fetchCfg.JqlQuery;
        const std::string projectKey =
            smatchet::ResolveProjectForDraft(app.GetTrackerBackend(), jql, std::string(), std::string());
        d.fieldCatalogFuture = StartFieldCatalogFetchAsync(app, fetchCfg, projectKey);
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
                // Push the fetched user list into the AppController cache so JQL
                // autocomplete can suggest assignees / reporters by display name.
                app.SetAvailableUsers(std::move(result.Users));
                d.fieldCatalogWarning = result.Warning;
                if (!d.fieldCatalogWarning.empty()) {
                    LOG_WARN("SmatchetUI: users fetch warning: %s", d.fieldCatalogWarning.c_str());
                }
            } else {
                app.SetFieldCatalog(
                    {}, {}, result.Error.empty() ? std::string("Failed to fetch field catalog.") : result.Error);
                app.SetAvailableUsers({});
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
        // In this branch `appliedInitialView` is already true (paired with the `if (!appliedInitialView)` above).
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

void SmatchetUI::drawMainMenuBar(AppController& app, UiDrawSession& d) {
    if (ImGui::BeginMainMenuBar()) {
        auto ticketsSnap = app.GetActiveTicketsSnapshot();
        const std::vector<CachedTicket> emptyTickets;
        const auto& tickets = ticketsSnap ? *ticketsSnap : emptyTickets;
        const TrackerFieldCatalogIndex& catalogIndex = *gridFrameCtx_.catalogIndex;
        const std::vector<TicketGridColumn>& columns = gridFrameCtx_.columns;
        const bool hasSelection = d.gridState.RectSel.HasAnySelection();
        const bool hasTickets = !tickets.empty();

        auto selectAllRows = [&]() {
            auto& sel = d.gridState.RectSel;
            sel.ClearAll();
            const size_t rowCount = !d.filteredIndices.empty() ? d.filteredIndices.size() : tickets.size();
            for (size_t row = 0; row < rowCount; ++row) {
                sel.Rows.insert(static_cast<int>(row));
            }
            if (rowCount > 0) {
                sel.PrimaryRow = 0;
                sel.SortSignature =
                    ComputeGridSortSignature(d.cachedSortFingerprint, d.cachedSortTicketsRevision, tickets.size());
                const size_t firstTicketIndex = !d.filteredIndices.empty() ? d.filteredIndices.front() : 0;
                if (firstTicketIndex < tickets.size()) {
                    d.gridState.ActiveIssueId = tickets[firstTicketIndex].id;
                }
            }
        };

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project View...", "Ctrl+O")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Issues...")) {
                d.showBulkImport = true;
            }
            if (ImGui::MenuItem("Export Issues...")) {
                d.showBulkExport = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Read-only Mode", nullptr, d.cfg.ReadOnlyMode, true)) {
                d.cfg.ReadOnlyMode = !d.cfg.ReadOnlyMode;
                ConfigManager::Save(d.cfg);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection && !columns.empty())) {
                CopyGridRectAsTsv(tickets, d.filteredIndices, columns, catalogIndex, d.gridState.RectSel);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Selection")) {
            if (ImGui::MenuItem("Select All", "Ctrl+A", false, hasTickets)) {
                selectAllRows();
            }
            if (ImGui::MenuItem("Clear Selection", "Ctrl+Shift+A", false, hasSelection)) {
                d.gridState.RectSel.ClearAll();
            }
            if (ImGui::MenuItem("Copy Selection", "Ctrl+Shift+C", false, hasSelection && !columns.empty())) {
                CopyGridRectAsTsv(tickets, d.filteredIndices, columns, catalogIndex, d.gridState.RectSel);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P")) {
                commandPalette_.Open();
            }
            if (ImGui::MenuItem("Open View...", "Ctrl+Shift+V")) {
                commandPalette_.Open();
                commandPalette_.SetFilterText("view.toggle.");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Appearance")) {
#ifndef SMATCHET_EMBEDDED_IN_UNREAL
                if (ImGui::MenuItem("Full Screen", "F11", d.cfg.FullScreen)) {
                    d.requestFullScreenToggle = true;
                }
#endif
                if (ImGui::MenuItem("Zen Mode", "Ctrl+M, Z", d.cfg.ZenMode)) {
                    d.cfg.ZenMode = !d.cfg.ZenMode;
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Theme")) {
                    struct ThemeEntry {
                        ThemeId id;
                        const char* label;
                    };
                    constexpr ThemeEntry kEntries[] = {
                        {ThemeId::SmatchetDark, "Smatchet Dark (default)"},
                        {ThemeId::ModernDark, "Modern Dark"},
                        {ThemeId::Vs2022Dark, "VS 2022 Dark"},
                        {ThemeId::Vs2022Light, "VS 2022 Light"},
                        {ThemeId::HighContrast, "High Contrast"},
                    };
                    for (const ThemeEntry& e : kEntries) {
                        if (ImGui::MenuItem(e.label, nullptr, d.cfg.Theme == e.id)) {
                            d.cfg.Theme = e.id;
                            ConfigManager::Save(d.cfg);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Density")) {
                    struct DensityEntry {
                        TrackerConfig::UiDensity id;
                        const char* label;
                    };
                    const DensityEntry kDensities[] = {
                        {TrackerConfig::UiDensity::Compact,     "Compact"},
                        {TrackerConfig::UiDensity::Normal,      "Normal"},
                        {TrackerConfig::UiDensity::Comfortable, "Comfortable"},
                    };
                    for (const DensityEntry& e : kDensities) {
                        if (ImGui::MenuItem(e.label, nullptr, d.cfg.Density == e.id)) {
                            d.cfg.Density = e.id;
                            ConfigManager::Save(d.cfg);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Font")) {
                    const char* kFonts[] = {
                        "Segoe UI", "Consolas", "Calibri", "Arial", "Cascadia Code", "JetBrains Mono",
                    };
                    const int kFontCount = static_cast<int>(sizeof(kFonts) / sizeof(kFonts[0]));
                    for (int fi = 0; fi < kFontCount; ++fi) {
                        const bool selected = (d.cfg.SelectedFontName == kFonts[fi]);
                        if (ImGui::MenuItem(kFonts[fi], nullptr, selected)) {
                            d.cfg.SelectedFontName = kFonts[fi];
                            ConfigManager::Save(d.cfg);
                            SmatchetRequestFontReload(d.cfg.SelectedFontName,
                                                      static_cast<float>(d.cfg.FontSizePt));
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Panel Position")) {
                    if (ImGui::MenuItem("Bottom", nullptr,
                                        d.cfg.PanelDockSide == TrackerConfig::PanelPosition::Bottom)) {
                        if (d.cfg.PanelDockSide != TrackerConfig::PanelPosition::Bottom) {
                            d.cfg.PanelDockSide = TrackerConfig::PanelPosition::Bottom;
                            ConfigManager::Save(d.cfg);
                            // Dock rebuild is complex and fragile; reset layout instead.
                            // The panel will be repositioned after layout reset on next launch.
                            resetWindowLayoutToDefault(d);
                        }
                    }
                    if (ImGui::MenuItem("Right", nullptr,
                                        d.cfg.PanelDockSide == TrackerConfig::PanelPosition::Right)) {
                        if (d.cfg.PanelDockSide != TrackerConfig::PanelPosition::Right) {
                            d.cfg.PanelDockSide = TrackerConfig::PanelPosition::Right;
                            ConfigManager::Save(d.cfg);
                            resetWindowLayoutToDefault(d);
                        }
                    }
                    ImGui::EndMenu();
                }
                {
                    const char* swapLabel = d.cfg.PrimarySideBarOnRight
                                                ? "Move Primary Side Bar Left"
                                                : "Move Primary Side Bar Right";
                    if (ImGui::MenuItem(swapLabel)) {
                        d.cfg.PrimarySideBarOnRight = !d.cfg.PrimarySideBarOnRight;
                        ConfigManager::Save(d.cfg);
                        resetWindowLayoutToDefault(d);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Zoom In", "Ctrl+=", false, d.cfg.FontSizePt < 32)) {
                    d.cfg.FontSizePt = (d.cfg.FontSizePt < 32) ? (d.cfg.FontSizePt + 1) : 32;
                    ConfigManager::Save(d.cfg);
                }
                if (ImGui::MenuItem("Zoom Out", "Ctrl+-", false, d.cfg.FontSizePt > 8)) {
                    d.cfg.FontSizePt = (d.cfg.FontSizePt > 8) ? (d.cfg.FontSizePt - 1) : 8;
                    ConfigManager::Save(d.cfg);
                }
                if (ImGui::MenuItem("Reset Zoom", "Ctrl+0", false, d.cfg.FontSizePt != 16)) {
                    d.cfg.FontSizePt = 16;
                    ConfigManager::Save(d.cfg);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Primary Side Bar", "Ctrl+B", d.cfg.ShowPrimarySideBar)) {
                    SetViewVisible(d.cfg, ViewSlot::PrimarySideBar, !d.cfg.ShowPrimarySideBar);
                    recentViews_.Touch("view.toggle.primary-side-bar");
                    ConfigManager::Save(d.cfg);
                }
                if (ImGui::MenuItem("Secondary Side Bar", "Ctrl+Alt+B", d.cfg.ShowSecondarySideBar)) {
                    SetViewVisible(d.cfg, ViewSlot::SecondarySideBar, !d.cfg.ShowSecondarySideBar);
                    recentViews_.Touch("view.toggle.secondary-side-bar");
                    ConfigManager::Save(d.cfg);
                }
                if (ImGui::MenuItem("Status Bar", nullptr, d.cfg.ShowStatusBar)) {
                    d.cfg.ShowStatusBar = !d.cfg.ShowStatusBar;
                    recentViews_.Touch("view.toggle.status-bar");
                    ConfigManager::Save(d.cfg);
                }
                if (ImGui::MenuItem("Panel", "Ctrl+J", d.cfg.ShowPanel)) {
                    SetViewVisible(d.cfg, ViewSlot::BottomPanel, !d.cfg.ShowPanel);
                    recentViews_.Touch("view.toggle.panel");
                    ConfigManager::Save(d.cfg);
                }
                ImGui::EndMenu();
            }
#if defined(SMATCHET_ENABLE_EDITOR_LAYOUT)
            if (ImGui::BeginMenu("Editor Layout")) {
                if (ImGui::MenuItem("Single"))        { /* TODO: DockBuilderSplitNode single */ }
                if (ImGui::MenuItem("Two Columns"))   { /* TODO */ }
                if (ImGui::MenuItem("Three Columns")) { /* TODO */ }
                if (ImGui::MenuItem("Two Rows"))      { /* TODO */ }
                if (ImGui::MenuItem("Grid (2x2)"))    { /* TODO */ }
                ImGui::EndMenu();
            }
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Views Dashboard", "Ctrl+Shift+E", d.showViewsDashboard)) {
                d.showViewsDashboard = !d.showViewsDashboard;
                if (d.showViewsDashboard) {
                    d.requestViewsDashboardFocus = true;
                }
                recentViews_.Touch("view.toggle.views-dashboard");
            }
            if (ImGui::MenuItem("Source Blame", "Ctrl+Shift+B", d.showBlameAnalysis)) {
                d.showBlameAnalysis = !d.showBlameAnalysis;
                recentViews_.Touch("view.toggle.source-blame");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Log", "Ctrl+Shift+U", d.showLogWindow)) {
                d.showLogWindow = !d.showLogWindow;
                recentViews_.Touch("view.toggle.log");
            }
            if (ImGui::MenuItem("Backend Audit", "Ctrl+Shift+M", d.showAuditTrail)) {
                d.showAuditTrail = !d.showAuditTrail;
                if (d.showAuditTrail) {
                    d.requestAuditTrailFocus = true;
                }
                recentViews_.Touch("view.toggle.backend-audit");
            }
            if (ImGui::MenuItem("Performance", nullptr, d.showPerformance)) {
                d.showPerformance = !d.showPerformance;
                recentViews_.Touch("view.toggle.performance");
            }
            if (ImGui::MenuItem("Bulk Import", nullptr, d.showBulkImport)) {
                d.showBulkImport = !d.showBulkImport;
                recentViews_.Touch("view.toggle.bulk-import");
            }
            if (ImGui::MenuItem("Bulk Export", nullptr, d.showBulkExport)) {
                d.showBulkExport = !d.showBulkExport;
                recentViews_.Touch("view.toggle.bulk-export");
            }
            if (ImGui::MenuItem("Preferences", nullptr, d.showPreferences)) {
                d.showPreferences = !d.showPreferences;
                recentViews_.Touch("view.toggle.preferences");
            }
#if defined(SMATCHET_WITH_MCP)
            if (ImGui::MenuItem("MCP Server", nullptr, d.showMcpServerWindow)) {
                d.showMcpServerWindow = !d.showMcpServerWindow;
                if (d.showMcpServerWindow) {
                    d.requestMcpServerFocus = true;
                }
                recentViews_.Touch("view.toggle.mcp-server");
            }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            if (ImGui::MenuItem("Scripts & Actions", nullptr, d.showLuaAutomationWindow)) {
                d.showLuaAutomationWindow = !d.showLuaAutomationWindow;
                if (d.showLuaAutomationWindow) {
                    d.requestLuaAutomationFocus = true;
                    d.requestScriptingEditorTabFocus = true;
                }
                recentViews_.Touch("view.toggle.scripts-and-actions");
            }
#endif
            ImGui::Separator();
            // Recently Used Views submenu: lists the last 5 toggled view ids, oldest first.
            if (ImGui::BeginMenu("Recently Used Views")) {
                const std::vector<std::string> recent = recentViews_.Snapshot();
                if (recent.empty()) {
                    ImGui::BeginDisabled();
                    ImGui::MenuItem("(none yet)");
                    ImGui::EndDisabled();
                } else {
                    for (int ri = static_cast<int>(recent.size()) - 1; ri >= 0; --ri) {
                        const std::string& cmdId = recent[static_cast<size_t>(ri)];
                        if (ImGui::MenuItem(cmdId.c_str())) {
                            smatchet::cmd::CommandContext ctx;
                            ctx.App = &app;
                            ctx.Source = smatchet::cmd::CommandSource::Palette;
                            const nlohmann::json emptyArgs = nlohmann::json::object();
                            smatchet::cmd::CommandResult r = app.Commands().Dispatch(cmdId, emptyArgs, ctx);
                            if (!r.Ok) {
                                LOG_DEBUG("SmatchetUI: recently used view command not found: %s", cmdId.c_str());
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Reset Layout")) {
                resetWindowLayoutToDefault(d);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Go")) {
            if (ImGui::MenuItem("Active Project Grid", "Ctrl+Alt+1")) {
                d.requestActiveProjectFocus = true;
            }
            if (ImGui::MenuItem("Views Dashboard", "Ctrl+Alt+2")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            if (ImGui::MenuItem("Log", "Ctrl+Alt+3")) {
                d.showLogWindow = true;
            }
            if (ImGui::MenuItem("Backend Audit", "Ctrl+Alt+4")) {
                d.showAuditTrail = true;
                d.requestAuditTrailFocus = true;
            }
            if (ImGui::MenuItem("Performance", "Ctrl+Alt+5")) {
                d.showPerformance = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Source Blame...")) {
                d.showBlameAnalysis = true;
            }
            ImGui::EndMenu();
        }
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        if (ImGui::BeginMenu("Run")) {
            if (ImGui::MenuItem("Scripts & Actions...")) {
                d.showLuaAutomationWindow = true;
                d.requestLuaAutomationFocus = true;
                d.requestScriptingEditorTabFocus = true;
            }
            ImGui::EndMenu();
        }
#endif
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Preferences...", "Ctrl+,")) {
                d.showPreferences = true;
            }
#if defined(SMATCHET_WITH_MCP)
            if (ImGui::MenuItem("MCP Server...")) {
                d.showMcpServerWindow = true;
                d.requestMcpServerFocus = true;
            }
#endif
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Check for Updates...", nullptr, false, !d.appUpdateCheckInFlight)) {
                StartAppUpdateCheck(d, app, true);
            }
            ImGui::EndMenu();
        }
#if !defined(SMATCHET_WITH_LUA_AUTOMATION)
        {
            static bool s_loggedLuaMenuAbsent = false;
            if (!s_loggedLuaMenuAbsent) {
                s_loggedLuaMenuAbsent = true;
                LOG_WARN("SmatchetUI: Lua automation disabled in this binary (no Scripts & Actions window).");
            }
        }
#endif

        // Inline Command Palette input — VS Code Quick Input.
        // Typing pre-fills + opens the existing palette modal; Enter does the same.
        {
            constexpr float kInlineMaxWidthPx = 640.0f;
            constexpr float kInlineMinWidthPx = 200.0f;
            constexpr float kRightReservedPx = 140.0f;
            const float menuRightEdge = ImGui::GetCursorPosX();
            const float rightLimit = ImGui::GetWindowContentRegionMax().x - kRightReservedPx;
            const float availW = (std::max)(0.0f, rightLimit - menuRightEdge);

            if (availW >= kInlineMinWidthPx) {
                const float inputW = (std::min)(kInlineMaxWidthPx, availW * 0.55f);
                const float xPad = (availW - inputW) * 0.5f;
                ImGui::SetCursorPosX(menuRightEdge + xPad);
                ImGui::SetNextItemWidth(inputW);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
                const bool committed = ImGui::InputTextWithHint("##cmd-palette-input", "Search commands (Ctrl+Shift+P)",
                                                                d.paletteInlineBuf, IM_ARRAYSIZE(d.paletteInlineBuf),
                                                                ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopStyleVar();
                if ((ImGui::IsItemActivated() || committed) && d.paletteInlineBuf[0] != '\0') {
                    if (!commandPalette_.IsOpen()) {
                        commandPalette_.Open();
                    }
                    commandPalette_.SetFilterText(d.paletteInlineBuf);
                }
                if (!commandPalette_.IsOpen() && !ImGui::IsItemActive() && d.paletteInlineBuf[0] != '\0') {
                    d.paletteInlineBuf[0] = '\0';
                }
            }
        }

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
