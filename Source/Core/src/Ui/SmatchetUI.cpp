#include "SmatchetUI.h"
#include "AppController.h"
#include "SmatchetViewVisibility.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetStatusBarUi.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/PaneCommands.h"
#include "Commands/ViewCommands.h"
#include "ConfigManager.h"
#include "TrackerGridFieldDisplay.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetImageTextureCache.h"
#include "SmatchetAttachmentPreviewUi.h"
#include "SmatchetTheme.h"
#include "SmatchetUiDensity.h"
#include "Logger.h"
#include "NavigationHistory.h"
#include "ProjectResolver.h"
#include "TicketGridModel.h"
#include "UiPerfMonitor.h"
#include "SmatchetPerfUi.h"
#include "SmatchetPlanDocViewerUi.h"
#include "SmatchetBugReportUi.h"
#include "ImGuiHotkey.h"
#include "SmatchetUiSession.h"
#include "Win32PickFiles.h"
#if defined(SMATCHET_WITH_MCP)
#include "SmatchetMcpServerUi.h"
#include "SmatchetAiAssistantUi.h"
#endif
#include "SmatchetToast.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#if defined(SMATCHET_WITH_WHISPER)
#include "SmatchetWhisperSetupBanner.h"
#include "SmatchetWhisperOverlayUi.h"
#include "DictationInsertionRouter.h"
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
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

// Forward decls for helpers shared with SmatchetUI_Layout.cpp / SmatchetUI_MainMenu.cpp.
// Declarations duplicated here (and in SmatchetUI_Internal.h) so this TU can keep using
// the raw `imgui.h` macro setup of SmatchetLocalizedImGui without dragging in the internal
// header's redefinitions (which clash with `imgui_internal.h` consumers further down).
namespace smatchet {
namespace ui_detail {
void PersistWindowOpenPreferences(UiDrawSession& d);
} // namespace ui_detail
} // namespace smatchet

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

namespace smatchet {
namespace ui_detail {

void StartAppUpdateCheck(UiDrawSession& d, AppController& app, bool manual) {
    if (d.appUpdateCheckInFlight) {
        return;
    }
    d.appUpdateActionStatus.clear();
    d.appUpdateCheckManual = manual;
    d.appUpdateCheckInFlight = true;
    d.appUpdateFuture = StartAppUpdateCheckAsync(app, d.cfg);
}

} // namespace ui_detail
} // namespace smatchet

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
        LOG_WARN("DrainAppUpdateCheck: unknown exception");
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
    if (d.installerDownloadInFlight) {
        ImGui::TextUnformatted("Downloading installer...");
        // Indeterminate progress bar — cpr doesn't expose byte-count callbacks without restructuring
        // the WriteCallback signature; the spinner satisfies the < 100ms-cue Pillar 2 contract.
        const float t = static_cast<float>(ImGui::GetTime());
        ImGui::ProgressBar(-1.0f * t, ImVec2(280.0f, 0.0f), "");
        ImGui::SameLine();
        if (ImGui::Button("Cancel##installer_cancel", ImVec2(90.0f, 0.0f))) {
            if (d.installerDownloadCancel) {
                d.installerDownloadCancel->store(true, std::memory_order_release);
            }
        }
    } else {
        if (ImGui::Button("Download and Install", ImVec2(170.0f, 0.0f))) {
            d.installerDownloadInFlight = true;
            d.installerDownloadCancel = std::make_shared<std::atomic<bool>>(false);
            const std::string downloadUrl = d.appUpdateInfo.InstallerAsset.DownloadUrl;
            const std::string assetName = d.appUpdateInfo.InstallerAsset.Name;
            std::shared_ptr<std::atomic<bool>> cancelFlag = d.installerDownloadCancel;
            d.appUpdateActionStatus = "Downloading installer...";
            app.LaunchBackgroundTask([&app, downloadUrl, assetName, cancelFlag]() {
                std::string err;
                const bool ok = app.DownloadAndLaunchInstallerUpdate(downloadUrl, assetName, err, cancelFlag);
                app.mainThreadDispatcher.PostToMainThread([ok, err]() {
                    g_ui.installerDownloadInFlight = false;
                    g_ui.installerDownloadCancel.reset();
                    if (ok) {
                        g_ui.appUpdateActionStatus =
                            "Installer launched. Smatchet will close so the update can proceed.";
                    } else {
                        g_ui.appUpdateActionStatus = err.empty() ? "Failed to launch installer update." : err;
                    }
                });
            });
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
    }
    ImGui::EndPopup();
}

static std::future<FieldCatalogFetchResult>
StartFieldCatalogFetchAsync(AppController& app, const TrackerConfig& fetchCfg, const std::string& projectKey) {
    return std::async(std::launch::async, [&app, fetchCfg, projectKey]() {
        FieldCatalogFetchResult result;
        result.BackendKey = ConfigManager::NormalizeViewsBackendKey(fetchCfg.TrackerType);
        result.ProjectKey = projectKey;
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

// `SmatchetUI::prepareTopLevelWindow`, `SmatchetUI::repairTopLevelWindow`,
// `SmatchetUI::resetWindowLayoutToDefault`, and the free function
// `SmatchetUI_ResetLayoutToDefault` moved to `SmatchetUI_Layout.cpp` per
// `docs/plans/shipped/large-files-and-phase-2.md` § A4.

void SmatchetUI::Draw(AppController& app) {
    UiDrawSession& d = g_ui;
    drawInitConfigOnce(app, d);
    drawApplyAppearanceSettings(d);
    drawPerFrameTicksAndHandlers(app, d);
    // Deferred view-create/-delete latches (Pillar 3 crash fix — see
    // UiDrawSession::viewsPendingCreate / viewsPendingDeleteActive): the store
    // mutations run HERE, before any ViewDefinition* is resolved for the frame, so
    // they can never invalidate a live pointer held by the dashboard / pane draw
    // contexts. Every Views store mutation that resizes the vector MUST go through
    // one of these latches — never mid-frame from a click handler.
    applyPendingViewCreate(d);
    applyPendingViewDelete(app, d);
    UiPerfMonitor::Instance().BeginFrame();
    SmatchetImageTextureCache::TickPendingDestroys();
    drawPreWindowOverlays(app, d);

    SMATCHET_UI_PERF_SCOPE("SmatchetUI::Draw");
    drawViewStateAndConnectivity(app, d);
    drawChromeAndModeToggles(app, d);
    handleViewKeyboardShortcuts(d);
    DrainAppUpdateCheck(d);
    drawSecondaryWindows(app, d);
    drawDockDebugOverlay(d);
    drawEndOfFramePersistence(d);
}

// One-time config load + layout-schema migration. Latches `cfgInitialized`; subsequent
// frames early-out.
void SmatchetUI::drawInitConfigOnce(AppController& app, UiDrawSession& d) {
    (void)app;
    (void)d; // body operates on the g_ui singleton directly (d aliases g_ui)
    if (g_ui.cfgInitialized) {
        return;
    }
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

// Per-frame zoom / density / theme re-application. All three are idempotent and only
// touch ImGui style when the corresponding cfg value drifts from the last applied value.
void SmatchetUI::drawApplyAppearanceSettings(UiDrawSession& d) {
    // Zoom: per-frame FontGlobalScale from cfg.FontSizePt. Cheap, instant, no atlas rebuild.
    ::ImGui::GetIO().FontGlobalScale = static_cast<float>(d.cfg.FontSizePt) / 16.0f;

    // Apply density padding — only re-apply when the setting changes.
    if (d.cfg.Density != lastAppliedDensity_) {
        lastAppliedDensity_ = d.cfg.Density;
        smatchet::ui_density::ApplyDensityToImGuiStyle(d.cfg.Density);
    }

    // Panel visibility is driven by the d.show* / cfg.Show* flags that gate each ImGui::Begin call.
    // ImGui collapses empty dock nodes automatically — no per-frame bit-manipulation needed.
    // (HiddenTabBar fights the layout on resize; removed in favour of the natural empty-node path.)

    // Re-apply the style palette only when cfg.Theme drifts from what is live in ImGui::GetStyle().
    // SmatchetImGuiHost seeds SmatchetDark before cfg is loaded; the first frame after Load() catches
    // any user-saved value through this check. ApplyStyle's ApplyCommonStyle rewrites ItemSpacing /
    // FramePadding to Normal-density defaults; re-push density so Compact / Comfortable survive.
    if (d.cfg.Theme != lastAppliedTheme_) {
        SmatchetTheme::ApplyStyle(d.cfg.Theme);
        lastAppliedTheme_ = d.cfg.Theme;
        smatchet::ui_density::ApplyDensityToImGuiStyle(d.cfg.Density);
    }
}

// Per-frame background work: startup banners, update-check kickoff, panel-status expiry,
// offline-queue ticks, and one-time attachment / open-file handler registration.
void SmatchetUI::drawPerFrameTicksAndHandlers(AppController& app, UiDrawSession& d) {
    if (d.cfgInitialized && !d.offlineLegacyStartupBannerConsumed) {
        d.offlineLegacyStartupBannerConsumed = true;
        d.offlineLegacyStartupBannerText = app.TakeLegacyPendingStartupBanner();
    }
    if (d.cfgInitialized && d.cfg.UpdateCheckEnabled && !d.appUpdateStartupCheckStarted) {
        d.appUpdateStartupCheckStarted = true;
        smatchet::ui_detail::StartAppUpdateCheck(d, app, false);
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
    app.TickAllContexts(); // all live pane contexts under one shared deadline (multi-grid Slice 3)
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
}

// Pre-window overlays that must run before any sub-window draw so they intercept key
// events first: the command palette, the bug-report hotkey + modal, and the scenario tick.
void SmatchetUI::drawPreWindowOverlays(AppController& app, UiDrawSession& d) {
    // Unified Command System: palette (Ctrl+Shift+P) — must run before any
    // sub-window draws so it can intercept key events first.
    // Skip while the first-launch tracker gate is active so the palette can't bypass it.
    if (g_ui.cfg.BackendHasBeenReachable) {
        // Honour the bucket-C scenario request to open + pre-filter the palette
        // before its Draw runs this frame. Consume-once: the scenario sets the
        // flag, we drain it here. Subsequent frames render the steady palette
        // state, which is what the screenshot diff golden captures.
        if (g_ui.requestCommandPaletteOpen) {
            g_ui.requestCommandPaletteOpen = false;
            commandPalette_.Open();
            if (!g_ui.requestCommandPaletteFilter.empty()) {
                commandPalette_.SetFilterText(g_ui.requestCommandPaletteFilter.c_str());
            }
        }
        commandPalette_.Draw(app);
    }

    // "Log a Bug" hotkey + modal — polled OUTSIDE the BackendHasBeenReachable gate
    // so bug reporting works even when the user's tracker is unreachable (the dev
    // repo is independent of the active backend).
    if (g_ui.cfg.BugReportHotkeyEnabled) {
        smatchet::ui::ImGuiBugHotkey hk;
        if (smatchet::ui::ParseImGuiHotkey(g_ui.cfg.BugReportHotkey, hk) &&
            smatchet::ui::MatchHotkey(::ImGui::GetIO(), hk)) {
            g_ui.showBugReport = true;
            g_ui.bugReportOpenLatch = true;
            // Opener owns the screenshot toggle — seed from config (the modal no longer does).
            g_ui.bugReportInclScreenshot = g_ui.cfg.BugReportScreenshotDefault;
        }
    }
    SmatchetBugReportUi_Draw(app, g_ui);

    // Scenario tick: drive the active scenario one frame and propagate scroll state
    // into the session so SmatchetActiveProjectGridUi can honor it.
    {
        bool scenScrollActive = false;
        int scenScrollTarget = -1;
        app.Scenarios().Tick(app, scenScrollActive, scenScrollTarget);
        d.scenarioScrollActive = scenScrollActive;
        d.scenarioScrollTarget = scenScrollTarget;
    }
}

// ViewState load + backend-key change reset, view-command registration, connectivity
// monitor tick, first-launch unlock latch, stale-banner clears, dispatcher drain,
// catalog/initial-sync, and the per-frame catalog+column cache rebuild.
void SmatchetUI::drawViewStateAndConnectivity(AppController& app, UiDrawSession& d) {
    {
        SMATCHET_UI_PERF_SCOPE("ViewState::EnsureLoaded");
        ViewState.EnsureLoaded(g_ui.cfg);
        const std::string bk = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        // SESSION-level backend-change detector (review HIGH-4): the state this reset
        // guards (catalog, initial sync, live context, view-editor buffers) is
        // session-scoped, so the delta must be too. Per-pane tracking went blind on
        // A→B→A pane-focus hops — the returning pane's own key never changed — and
        // the reset never fired. A pane focus switch re-points cfg.TrackerType, so
        // this fires on host focus switches as well.
        std::string& lastViewsBackendKey = d.lastViewsBackendKey;
        if (!lastViewsBackendKey.empty() && lastViewsBackendKey != bk) {
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
        lastViewsBackendKey = bk;
    }
    // Register view.* commands once ViewState is loaded (idempotent — skips on 2nd+ call).
    smatchet::cmd::RegisterViewCommands(app, ViewState);
    // Register pane.* commands (multi-grid Slice 4) — same idempotent guard; the grid-pane
    // state lives on the UiDrawSession singleton, so pass the live session in.
    smatchet::cmd::RegisterPaneCommands(app, d);
    {
        SMATCHET_UI_PERF_SCOPE("TickTrackerConnectivityMonitor");
        app.TickTrackerConnectivityMonitor(g_ui.cfg);
    }
    // First-launch gate: unlock the main UI permanently once the backend has been
    // confirmed reachable. Latches true forever; never re-locks on later launches.
    if (!g_ui.cfg.BackendHasBeenReachable &&
        app.GetLastTrackerConnectivityState() == AppController::TrackerConnectivityState::AuthenticatedReachable) {
        g_ui.cfg.BackendHasBeenReachable = true;
        ConfigManager::Save(g_ui.cfg);
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
}

// Menu bar, toolbar, Whisper banner/overlay, dock-debug toggle, status bar, and the
// fullscreen / Zen-mode chrome toggles. Zen key-chord + esc-esc state hoisted to
// drawBodyState_ (was function-local statics).
void SmatchetUI::drawChromeAndModeToggles(AppController& app, UiDrawSession& d) {
    if (!d.cfg.ZenMode) {
        SMATCHET_UI_PERF_SCOPE("drawMainMenuBar");
        drawMainMenuBar(app, d);
    }
    if (!d.cfg.ZenMode) {
        SMATCHET_UI_PERF_SCOPE("SmatchetToolbarUi::Draw");
        toolbar_.Draw(app, d.cfg);
    }
#if defined(SMATCHET_WITH_WHISPER)
    // Whisper first-run dictation setup banner — pinned under the menu bar,
    // visible only while cfg.WhisperSetupCompleted == false. Renders nothing
    // once the user has answered (Enable / No thanks). See
    // docs/plans/shipped/whisper-dictation.md § Setup banner spec.
    if (!d.cfg.WhisperSetupCompleted && !d.cfg.ZenMode) {
        SMATCHET_UI_PERF_SCOPE("SmatchetWhisperSetupBanner::Render");
        if (smatchet::whisper::banner::Render(app, d.cfg)) {
            ConfigManager::Save(d.cfg);
        }
    }
    // Phase E — floating amplitude-meter overlay while push-to-talk capture is
    // active. Cheap when idle (single atomic load + early return); when active
    // draws one ImGui::ProgressBar + a Cancel button.
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetWhisperOverlayUi::Render");
        smatchet::whisper::overlay::Render(app);
    }
#endif
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

    // Zen Mode: Ctrl+M then Z chord (1 s timeout). Esc Esc to exit. Chord state lives in
    // drawBodyState_ (hoisted from the former function-local `static KeyChord s_zenChord`).
    {
        static const float kZenChordTimeout = 1.0f;
        const ImGuiIO& zcIo = ::ImGui::GetIO();
        const bool ctrlM = zcIo.KeyCtrl && !zcIo.KeyShift && !zcIo.KeyAlt && ::ImGui::IsKeyPressed(ImGuiKey_M, false);
        const bool keyZ = !zcIo.KeyCtrl && !zcIo.KeyShift && !zcIo.KeyAlt && ::ImGui::IsKeyPressed(ImGuiKey_Z, false);
        bool zenChordFired = false;
        if (ctrlM) {
            drawBodyState_.zenChordPrefixArmed = true;
            drawBodyState_.zenChordTimeoutSec = 0.0f;
        }
        if (drawBodyState_.zenChordPrefixArmed) {
            drawBodyState_.zenChordTimeoutSec += zcIo.DeltaTime;
            if (drawBodyState_.zenChordTimeoutSec > kZenChordTimeout) {
                drawBodyState_.zenChordPrefixArmed = false;
            }
            if (zcIo.WantTextInput) {
                drawBodyState_.zenChordPrefixArmed = false;
            }
            if (keyZ && drawBodyState_.zenChordPrefixArmed) {
                drawBodyState_.zenChordPrefixArmed = false;
                zenChordFired = true;
            }
        }
        if (zenChordFired) {
            d.cfg.ZenMode = !d.cfg.ZenMode;
        }
    }
    // Esc Esc to exit Zen Mode. Counter / timer hoisted to drawBodyState_.
    if (d.cfg.ZenMode) {
        drawBodyState_.zenEscTimer += ::ImGui::GetIO().DeltaTime;
        if (drawBodyState_.zenEscTimer > 0.5f) {
            drawBodyState_.zenEscCount = 0;
            drawBodyState_.zenEscTimer = 0.0f;
        }
        if (::ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ++drawBodyState_.zenEscCount;
            drawBodyState_.zenEscTimer = 0.0f;
            if (drawBodyState_.zenEscCount >= 2) {
                d.cfg.ZenMode = false;
                drawBodyState_.zenEscCount = 0;
            }
        }
    }
}

// Keyboard shortcuts for panel-visibility toggles + always-reveal view-menu shortcuts.
// Pure key-dispatch; no positional-ImGui pair. Split into two sub-helpers to stay under
// the branch cap.
void SmatchetUI::handleViewKeyboardShortcuts(UiDrawSession& d) {
    handlePanelVisibilityShortcuts(d);
    handleViewRevealShortcuts(d);
}

// Panel-visibility toggles (Ctrl+B primary side bar / Ctrl+Alt+B secondary side bar /
// Ctrl+J bottom panel). Each persists immediately.
void SmatchetUI::handlePanelVisibilityShortcuts(UiDrawSession& d) {
    // ImGui::Shortcut available in docking branch; fall back to GetIO check if absent.
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

// View-menu global always-reveal shortcuts (Ctrl+Shift+{A,F,D,I,X,K,L} / Ctrl+,). Setting
// show* = true + raising the focus latch every press lets the consumer's wantFocus-driven
// SetNextWindowFocus-before-Begin activate the docked tab.
void SmatchetUI::handleViewRevealShortcuts(UiDrawSession& d) {
    const ImGuiIO& io = ::ImGui::GetIO();
    const bool ctrlDown = io.KeyCtrl;
    const bool altDown = io.KeyAlt;
    const bool shiftDown = io.KeyShift;
    const bool ctrlShiftOnly = ctrlDown && shiftDown && !altDown;
#if defined(SMATCHET_WITH_AI)
    // Ctrl+Shift+A always-reveals the Smatchet Assistant side panel. Persistence runs
    // through the panel-draw path (PersistOpenStateImmediate). Always-reveal-on-shortcut
    // contract (AGENTS.md): re-pressing while open must focus, not close. Closing happens
    // via the X button only.
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        d.assistantPanelOpen = true;
        d.requestAssistantFocus = true;
    }
#endif
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        d.showPerformance = true;
        d.requestPerformanceFocus = true;
    }
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        d.showPlanDocViewer = true;
        d.requestPlanDocViewerFocus = true;
    }
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        d.showBulkImport = true;
        d.requestBulkImportFocus = true;
    }
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        d.showBulkExport = true;
        d.requestBulkExportFocus = true;
    }
    // Ctrl+, (no shift, no alt) — canonical IDE Preferences shortcut.
    if (ctrlDown && !shiftDown && !altDown && ::ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
        d.showPreferences = true;
        d.requestPreferencesFocus = true;
    }
#if defined(SMATCHET_WITH_MCP)
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        d.showMcpServerWindow = true;
        d.requestMcpServerFocus = true;
    }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    if (ctrlShiftOnly && ::ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        d.showLuaAutomationWindow = true;
        d.requestLuaAutomationFocus = true;
        d.requestScriptingEditorTabFocus = true;
    }
#endif
}

// All secondary windows + the legacy startup banner, drawn after the menu bar / toolbar.
// Each pre-existing SMATCHET_UI_PERF_SCOPE seam is reused verbatim.
void SmatchetUI::drawSecondaryWindows(AppController& app, UiDrawSession& d) {
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
            // Unified focus path: hand wantFocus to DrawWindow which applies the
            // SetNextWindowFocus-before-Begin + SetWindowFocus-after-Begin pattern internally.
            const bool wantPerfFocus = g_ui.requestPerformanceFocus;
            g_perfUi.DrawWindow(&g_ui.showPerformance, wantPerfFocus);
            if (wantPerfFocus) {
                g_ui.requestPerformanceFocus = false;
                LOG_DEBUG("Performance window: focused via menu request");
            }
        } else if (g_ui.requestPerformanceFocus) {
            // Defensive: clear the latch if the window is somehow disabled before consumption.
            g_ui.requestPerformanceFocus = false;
        }
    }
    if (g_ui.showAnnotateAnalysis) {
        annotateAnalysisUi_.DrawWindow(app, &g_ui.showAnnotateAnalysis, g_ui.focusedPane().gridState.ActiveIssueId);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawPreferencesWindow");
        drawPreferencesWindow(app, d);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawViewsDashboardWindow");
        // First-launch gate: hide Views & Queries until backend connection works.
        if (d.cfg.BackendHasBeenReachable) {
            drawViewsDashboardWindow(app, d);
        }
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawActiveProjectWindow");
        // Slice 2 (multi-grid-tabs): one dockable window PER GridPane; the host loops
        // the re-entrant drawActiveProjectWindow over d.gridPanes.
        drawGridPaneWindows(app, d);
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
        SMATCHET_UI_PERF_SCOPE("DrawPlanDocViewer");
        smatchet::DrawPlanDocViewer(d);
    }
    drawSecondaryWindowsTail(app, d);
}

// Tail half of drawSecondaryWindows: toasts, update modal, audit, AI assistant, watchers/votes
// list windows, MCP server, log window, FPS overlay. Split out for function-size compliance.
void SmatchetUI::drawSecondaryWindowsTail(AppController& app, UiDrawSession& d) {
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetToastManager::Render");
        SmatchetToastManager::Instance().Render();
    }
    DrawAppUpdateModal(app, d);
    {
        SMATCHET_UI_PERF_SCOPE("drawAuditWindow");
        drawAuditWindow(app, d);
    }
#if defined(SMATCHET_WITH_AI)
    {
        SMATCHET_UI_PERF_SCOPE("drawAiAssistantPanel");
        drawAiAssistantPanel(app, d);
    }
#endif
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
}

// Dock-node debug overlay — toggled by Ctrl+Alt+D. Owns its own ::ImGui::Begin/End pair.
// LOG_DEBUG throttle counter hoisted to drawBodyState_.
void SmatchetUI::drawDockDebugOverlay(UiDrawSession& d) {
    if (!d.showDockDebug) {
        return;
    }
    const ImGuiWindowFlags kDbgFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
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
        ::ImGui::Text("%s: size=(%.0f,%.0f) flags=0x%X tabs=%d empty=%d", kNames[i], node->Size.x, node->Size.y,
                      static_cast<int>(node->LocalFlags), node->Windows.Size, static_cast<int>(node->IsEmpty()));
    }
    ::ImGui::Separator();
    ::ImGui::Text("ShowPrimary=%d ShowPanel=%d ShowSecondary=%d", d.cfg.ShowPrimarySideBar, d.cfg.ShowPanel,
                  d.cfg.ShowSecondarySideBar);
    ::ImGui::End();

    // Per-frame LOG_DEBUG throttled to every 120 frames.
    ++drawBodyState_.dockDebugLogFrame;
    if (drawBodyState_.dockDebugLogFrame >= 120) {
        drawBodyState_.dockDebugLogFrame = 0;
        for (int i = 0; i < 3; ++i) {
            ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(kNodes[i]);
            if (!node) {
                LOG_DEBUG("DockDebug: %s NOT FOUND", kNames[i]);
            } else {
                LOG_DEBUG("DockDebug: %s size=(%.0f,%.0f) flags=0x%X tabs=%d empty=%d", kNames[i], node->Size.x,
                          node->Size.y, static_cast<int>(node->LocalFlags), node->Windows.Size,
                          static_cast<int>(node->IsEmpty()));
            }
        }
        LOG_DEBUG("DockDebug: ShowPrimary=%d ShowPanel=%d ShowSecondary=%d", d.cfg.ShowPrimarySideBar, d.cfg.ShowPanel,
                  d.cfg.ShowSecondarySideBar);
    }
}

// End-of-frame coalesced persistence: debounced ViewState save, window-open prefs,
// forced layout-defaults ini flush, and the debounced prefs ConfigManager::Save.
void SmatchetUI::drawEndOfFramePersistence(UiDrawSession& d) {
    (void)d;
    // Skip the debounced auto-save while a view edit is pending an explicit Save —
    // widths / sort specs mutated under the unsaved-layout strip must not bleed
    // through to disk until the user commits.
    if (g_ui.pendingViewStateSave && !g_ui.viewsDirty &&
        std::chrono::steady_clock::now() >= g_ui.pendingViewStateSaveAt) {
        SMATCHET_UI_PERF_SCOPE("ViewState::SaveDebounced");
        ViewState.Save();
        g_ui.pendingViewStateSave = false;
    }
    smatchet::ui_detail::PersistWindowOpenPreferences(g_ui);
    if (g_ui.layoutForceDefaultsFrames > 0) {
        --g_ui.layoutForceDefaultsFrames;
        if (g_ui.layoutForceDefaultsFrames == 0) {
            ImGui::SaveIniSettingsToDisk(ConfigManager::GetImGuiSettingsPath().c_str());
        }
    }
    // Coalesced ConfigManager::Save for SmatchetPreferencesUi widget mutations.
    // Each MarkPrefsDirty call arms `prefsDirty` + a ~100 ms debounce window;
    // we drain at end-of-frame (after all panels have drawn) so the write
    // happens outside any mid-panel state. See SmatchetUiSession.h MarkPrefsDirty
    // and docs/plans/shipped/pillar-1-2-audit-2026-05-17.md § H11 + § Pillar 1 P1.
    if (g_ui.prefsDirty && std::chrono::steady_clock::now() >= g_ui.prefsSaveDueAt) {
        SMATCHET_UI_PERF_SCOPE("ConfigManager::Save (prefs-debounced)");
        ConfigManager::Save(g_ui.cfg);
        g_ui.prefsDirty = false;
    }
}

#if defined(SMATCHET_WITH_AI)
void SmatchetUI::drawAiAssistantPanel(AppController& app, UiDrawSession& d) {
    // Free function lives in SmatchetAiAssistantUi.cpp; this member exists to keep
    // SmatchetUI.h's private-method contract uniform with the other window drawers.
    // Phase C: forward the active view definition so the panel's auto-context builder
    // can populate the ActiveView block.
    SmatchetDrawAiAssistantPanel(app, d, ViewState.GetActiveView());
}
#endif

void SmatchetUI::drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d) {
    const auto startCatalogFetch = [&](const TrackerConfig& fetchCfg) {
        if (d.fieldCatalogLoading) {
            return;
        }
        d.fieldCatalogLoading = true;
        d.fieldCatalogFetchStarted = true;
        const ViewDefinition* activeView = ViewState.GetActiveView();
        const std::string jql = activeView ? activeView->Jql : fetchCfg.JqlQuery;
        const ITrackerBackend* pb = app.GetTrackerBackend();
        const std::string projectKey =
            smatchet::ResolveProjectForDraft(pb ? &pb->Connectivity() : nullptr, jql, std::string(), std::string());
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
                // Pin the project key the fetch was scoped to so the snapshot saves under the
                // project-scoped cache entry (which carries Phase-3 component options), not the
                // unscoped ("") key. Without this the scoped result is lost on next startup and
                // the components column renders as text. See AppController::SetCurrentCatalogProject.
                app.SetCurrentCatalogProject(result.ProjectKey);
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

// `SmatchetUI::drawMainMenuBar` moved to `SmatchetUI_MainMenu.cpp` per
// `docs/plans/shipped/large-files-and-phase-2.md` § A4. Only call site is
// `SmatchetUI::Draw` above (the declaration stays in `SmatchetUI.h`).

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

// `DrainUiDrawSessionFuturesBeforeAppTeardown` and `UiDrawSession::~UiDrawSession`
// moved to `SmatchetUI_Layout.cpp` per `docs/plans/shipped/large-files-and-phase-2.md` § A4.
