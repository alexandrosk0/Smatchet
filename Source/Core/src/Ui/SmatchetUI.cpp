// SMATCHET_DEVIATION(rule=duplication; reason=include-block clone, audit #12; owner=cpp-audit; revisit=2026-09-30)
#include "SmatchetUI.h"
#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
// SMATCHET_DEVIATION(rule=duplication; reason=include-block clone, audit #12; owner=cpp-audit; revisit=2026-09-30)
#include "SmatchetViewVisibility.h"
// SMATCHET_DEVIATION(rule=duplication; reason=include-block clone, audit #12; owner=cpp-audit; revisit=2026-09-30)
#include "SmatchetDockNodeIds.h"
#include "SmatchetStatusBarUi.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "Commands/PaletteOpenLatch.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/PaneCommands.h"
#include "Commands/ViewCommands.h"
#include "ConfigManager.h"
#include "ConfigSaveWorker.h"
#include "Json/BoundedJsonParse.h"
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
#include "SmatchetViewsDashboardUi_detail.h"
#include "SmatchetBugReportUi.h"
#include "SmatchetQuickCreateIssueUi.h"
#include "ImGuiHotkey.h"
#include "SmatchetUiSession.h"
#include "Win32PickFiles.h"
#if defined(SMATCHET_WITH_MCP)
#include "SmatchetMcpServerUi.h"
#include "SmatchetAiAssistantUi.h"
#endif
#include "SmatchetToast.h"
#include "SmatchetNotificationCenterUi.h"
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
#include <atomic>
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
    const char* const checkFailedMsg = SmatchetLocalization::T(
        "updates.check_failed", "Update check failed — check your network connection and try again. You can "
                                "also download releases from the GitHub page.");
    try {
        d.appUpdateInfo = d.appUpdateFuture.get();
    } catch (const std::exception& ex) {
        LOG_WARN("DrainAppUpdateCheck: %s", ex.what());
        d.appUpdateInfo = {};
        d.appUpdateInfo.Error = checkFailedMsg;
    } catch (...) {
        LOG_WARN("DrainAppUpdateCheck: unknown exception");
        d.appUpdateInfo = {};
        d.appUpdateInfo.Error = checkFailedMsg;
    }

    if (!d.appUpdateInfo.Error.empty()) {
        if (d.appUpdateCheckManual) {
            SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.updates", "Updates"),
                                                  d.appUpdateInfo.Error, ToastType::Error, 5000);
        }
        return;
    }
    if (!d.appUpdateInfo.UpdateAvailable) {
        if (d.appUpdateCheckManual) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.updates", "Updates"),
                SmatchetLocalization::T("updates.already_latest", "You are already on the latest release."),
                ToastType::Success, 3500);
        }
        return;
    }
    if (!d.appUpdateCheckManual && !d.cfg.UpdateSkipVersion.empty() &&
        d.cfg.UpdateSkipVersion == d.appUpdateInfo.LatestVersion) {
        return;
    }

    d.appUpdateModalOpen = true;
    ImGui::OpenPopup("Update Available###AppUpdateAvailable");
    if (d.appUpdateCheckManual) {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.updates", "Updates"),
                                              SmatchetLocalization::T("updates.new_version", "New version found."),
                                              ToastType::Info, 2500);
    }
}

static void DrawAppUpdateModal(AppController& app, UiDrawSession& d) {
    if (d.appUpdateModalOpen) {
        ImGui::OpenPopup("Update Available###AppUpdateAvailable");
    }
    if (!ImGui::BeginPopupModal("Update Available###AppUpdateAvailable", &d.appUpdateModalOpen,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
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
    ImGui::TextWrapped("A newer Smatchet release is available on GitHub.");
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
        // Auto-size: the label translates, so a fixed 170px width clips the French text.
        if (ImGui::Button("Download and Install")) {
            d.installerDownloadInFlight = true;
            d.installerDownloadCancel = std::make_shared<std::atomic<bool>>(false);
            const std::string downloadUrl = d.appUpdateInfo.InstallerAsset.DownloadUrl;
            const std::string assetName = d.appUpdateInfo.InstallerAsset.Name;
            std::shared_ptr<std::atomic<bool>> cancelFlag = d.installerDownloadCancel;
            d.appUpdateActionStatus =
                SmatchetLocalization::T("updates.downloading_installer", "Downloading installer...");
            app.LaunchBackgroundTask([&app, downloadUrl, assetName, cancelFlag]() {
                std::string err;
                const bool ok = app.DownloadAndLaunchInstallerUpdate(downloadUrl, assetName, err, cancelFlag);
                app.PostToMainThread([ok, err]() {
                    g_ui.installerDownloadInFlight = false;
                    g_ui.installerDownloadCancel.reset();
                    if (ok) {
                        g_ui.appUpdateActionStatus = SmatchetLocalization::T(
                            "updates.installer_launched",
                            "Installer launched. Smatchet will close so the update can proceed.");
                    } else if (err.empty()) {
                        g_ui.appUpdateActionStatus = SmatchetLocalization::T("updates.installer_launch_failed",
                                                                             "Failed to launch installer update.");
                    } else {
                        g_ui.appUpdateActionStatus = err;
                    }
                });
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip This Version")) {
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
StartFieldCatalogFetchAsync(AppController& app, const TrackerConfig& fetchCfg, const std::string& activeViewJql) {
    // Latch backend on the UI thread: focusedContext().Backend can become null while the worker
    // runs (multi-grid Slice 3 — focus moves to a freshly spun-up pane before its sync swaps
    // a backend in). FetchFieldCatalog re-reads focusedContext() and would fail spuriously.
    const std::shared_ptr<ITrackerBackend> latchedBackend = app.BackendShared();
    return std::async(std::launch::async, [fetchCfg, activeViewJql, latchedBackend]() {
        FieldCatalogFetchResult result;
        result.BackendKey = ConfigManager::NormalizeViewsBackendKey(fetchCfg.TrackerType);
        std::string projectKey;
        const std::shared_ptr<ITrackerBackend> backend = latchedBackend;
        if (fetchCfg.TrackerType == "Plane" && backend != nullptr) {
            projectKey =
                smatchet::ResolvePlaneOperationProject(&backend->Connectivity(), activeViewJql, fetchCfg.JqlQuery);
        } else {
            projectKey = smatchet::ResolveProjectForDraft(backend ? &backend->Connectivity() : nullptr, activeViewJql,
                                                          std::string(), std::string());
        }
        result.ProjectKey = projectKey;
        std::string error;
        TrackerFieldCatalogResult catalog;
        if (!backend) {
            error = "Tracker backend is not initialized.";
            result.Ok = false;
            result.Error = error;
            return result;
        }
        if (!backend->FieldCatalog()) {
            error = "FetchFieldCatalog is not supported by this backend.";
            result.Ok = false;
            result.Error = error;
            return result;
        }
        const auto catalogResult = backend->FieldCatalog()->FetchFieldCatalog(fetchCfg, projectKey);
        result.Ok = static_cast<bool>(catalogResult);
        if (!result.Ok) {
            result.Error = catalogResult.error().Detail;
            result.ErrorTransient = catalogResult.error().IsRetryable();
            return result;
        }
        catalog = std::move(catalogResult.value());
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
    drawResolveUiMode(d);
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
    // Per-frame view/connectivity state (incl. the MainThreadDispatcher drain that runs
    // worker-thread callbacks) must execute in BOTH modes — so it stays ahead of the fork.
    drawViewStateAndConnectivity(app, d);

    // Mobile fork (dual-ui-mode-desktop-mobile plan, slice 3). The host viewport dockspace
    // is already created by the host layer; the mobile shell is a fullscreen window drawn
    // on top of it. Skip the entire desktop chrome + docked-window path; global overlays
    // (toasts + update modal) and end-of-frame persistence still run.
    if (d.effectiveUiMode == EffectiveUiMode::Mobile) {
        drawMobileShell(app, d);
        drawGlobalOverlays(app, d);
        drawEndOfFramePersistence(d);
        return;
    }

    // Mobile->Desktop edge (slice 5): if the previous frame ran the mobile shell it
    // detached io.IniFilename and routed saves to imgui_mobile.ini; re-attach the desktop
    // ini + reload it before any desktop window submits so dock geometry comes back.
    drawMobileRestoreDesktopIni(d);

    drawChromeAndModeToggles(app, d);
    // Global Chrome-omnibox search bar (jql-omnibox plan, Stream B). Drawn after the chrome
    // and before the docked windows so its BeginViewportSideBar(Up) strip shrinks the work
    // area the grid panes dock into (mechanism A — no host/frame-loop edit).
    drawOmnibar(app, d);
    DrainAppUpdateCheck(d);
    drawSecondaryWindows(app, d);
    drawDockDebugOverlay(d);
    // Shared "Set shortcut..." quick-bind modal. Drawn late (top level, after the toolbar
    // + palette have submitted their context menus this frame) so a request latched this
    // frame opens the modal this frame, and the modal stacks above all docked windows.
    drawQuickBindPopup(app, d);
    drawEndOfFramePersistence(d);
}

// drawResolveUiMode + the mobile shell methods + the Auto-mode width-hysteresis consts
// live in SmatchetMobileShellUi.cpp (dual-ui-mode-desktop-mobile plan, slice 3).

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
    // Config (incl. cfg.Keybindings) just loaded — force the shortcut parse cache to
    // rebuild from it on the first dispatchKeybindings this frame.
    keybindingCacheDirty_ = true;
    ApplyLoggingSettingsFromConfig(g_ui.cfg);

    if (!g_ui.cfg.SelectedFontName.empty() && g_ui.cfg.SelectedFontName != "Segoe UI") {
        SmatchetRequestFontReload(g_ui.cfg.SelectedFontName, 16.0f);
    }
}

// Per-frame zoom / density / theme re-application. All three are idempotent and only
// touch ImGui style when the corresponding cfg value drifts from the last applied value.
void SmatchetUI::drawApplyAppearanceSettings(UiDrawSession& d) {
    // Zoom: per-frame FontGlobalScale from cfg.FontSizePt. Cheap, instant, no atlas rebuild.
    // In Mobile the touch-density preset multiplies the base zoom so glyphs reach finger size
    // (dual-ui-mode slice 8); this is idempotent so it runs every frame and auto-reverts on the
    // flip back to Desktop. The matching size/spacing scale (ScaleAllSizes, non-idempotent) is
    // applied once per flip in the dirty gate below.
    float fontScale = static_cast<float>(d.cfg.FontSizePt) / 16.0f;
    if (d.effectiveUiMode == EffectiveUiMode::Mobile) {
        fontScale *= mobileTouchDensityScale(d.cfg.MobileTouchDensity);
    }
    ::ImGui::GetIO().FontGlobalScale = fontScale;

    // Apply density padding — only re-apply when the setting changes.
    if (d.cfg.Density != lastAppliedDensity_) {
        lastAppliedDensity_ = d.cfg.Density;
        smatchet::ui_density::ApplyDensityToImGuiStyle(d.cfg.Density);
    }

    // Panel visibility is driven by the d.show* / cfg.Show* flags that gate each ImGui::Begin call.
    // ImGui collapses empty dock nodes automatically — no per-frame bit-manipulation needed.
    // (HiddenTabBar fights the layout on resize; removed in favour of the natural empty-node path.)

    // Style-rebuild dirty gate (theme + dual-ui-mode slice 8 touch scaling, unified). ApplyStyle is
    // the one path that rewrites the whole style from baseline (ApplyCommonStyle resets ItemSpacing /
    // FramePadding to Normal-density and ReapplyHostDensityScale re-asserts the HOST density base), so
    // EVERY trigger that needs a rebuild must run through this single gate and then re-establish the
    // FULL layered scale stack — otherwise one trigger (e.g. a theme change while already in Mobile)
    // rebuilds to host base and silently drops the touch enlargement another trigger had composed.
    // Triggers: cfg.Theme drift (host seeds SmatchetDark pre-cfg; first frame after Load catches the
    // saved value), an effective-mode flip, or a MobileTouchDensity change while already in Mobile.
    // ScaleAllSizes is multiplicative (non-idempotent), so this must fire ONCE per change, never every
    // frame. After the rebuild: in Mobile, COMPOSE the touch scale via ApplyTouchScale — a pure
    // live-style multiply that never writes g_hostDensityScale, so net Mobile scale is host*touch and
    // the host base survives untouched (Android ~2.6 / Windows-HiDPI 1.6 stay intact — the Auto
    // logical-width divisor + mobile band heights read HostDensityScale()); in Desktop, re-push the
    // user's density spacing (touchScale would be 1.0 / inert anyway) so the flip back is identical.
    const bool mobileNow = (d.effectiveUiMode == EffectiveUiMode::Mobile);
    const bool themeChanged = (d.cfg.Theme != lastAppliedTheme_);
    const bool modeChanged = (d.effectiveUiMode != lastAppliedEffectiveUiMode_);
    const bool densityChangedInMobile = mobileNow && (d.cfg.MobileTouchDensity != lastAppliedMobileDensity_);
    if (themeChanged || modeChanged || densityChangedInMobile) {
        SmatchetTheme::ApplyStyle(d.cfg.Theme);
        lastAppliedTheme_ = d.cfg.Theme;
        if (mobileNow) {
            // Compose touch enlargement on top of the host base ApplyStyle just re-asserted.
            SmatchetTheme::ApplyTouchScale(mobileTouchDensityScale(d.cfg.MobileTouchDensity));
        } else {
            // Desktop restores the user's density spacing onto the (host-base) rebuilt style.
            smatchet::ui_density::ApplyDensityToImGuiStyle(d.cfg.Density);
        }
        lastAppliedEffectiveUiMode_ = d.effectiveUiMode;
        lastAppliedMobileDensity_ = d.cfg.MobileTouchDensity;
    }
}

// Per-frame background work: startup banners, update-check kickoff, panel-status expiry,
// offline-queue ticks, and one-time attachment / open-file handler registration.
void SmatchetUI::drawPerFrameTicksAndHandlers(AppController& app, UiDrawSession& d) {
    if (d.cfgInitialized && !d.offlineLegacyStartupBannerConsumed) {
        d.offlineLegacyStartupBannerConsumed = true;
        d.offlineLegacyStartupBannerText = app.TakeLegacyPendingStartupBanner();
        // One-shot corrupt-config notice (previously log-only: settings reverted to defaults
        // with zero UI signal). Same startup seam as the legacy banner; UI thread only.
        const std::string badConfigName = ConfigManager::TakeStartupConfigWarning();
        if (!badConfigName.empty()) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.settings", "Settings"),
                SmatchetLocalization::Format(
                    "config.parse_failed",
                    "Your settings file (%s) could not be read, so defaults are in use. Fix the file, or open "
                    "Preferences and Save to rewrite it.",
                    badConfigName.c_str()),
                ToastType::Warning, 12000);
        }
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
// events first: the rebindable keyboard-shortcut dispatch, the command palette, the
// bug-report modal, and the scenario tick.
void SmatchetUI::drawPreWindowOverlays(AppController& app, UiDrawSession& d) {
    // Rebindable keyboard shortcuts (replaces the former hardcoded handle*Shortcuts plus
    // the inline dock-debug Ctrl+Alt+D, F11, palette-toggle, and bug-report polls). Runs
    // first so a bound hotkey is dispatched before any sub-window can swallow the key
    // event. ui.command_palette is special-cased inside dispatchKeybindings so it can
    // self-gate on BackendHasBeenReachable.
    dispatchKeybindings(app, d);

    // Unified Command System: palette window render. The Ctrl+Shift+P open/close is
    // dispatched above (ui.command_palette pseudo-binding); this block only renders the
    // palette. Skip while the first-launch tracker gate is active so it can't bypass it.
    if (g_ui.cfg.BackendHasBeenReachable) {
        // Honour the bucket-C scenario request to open + pre-filter the palette
        // before its Draw runs this frame. Consume-once: the scenario sets the
        // flag, we drain it here. Subsequent frames render the steady palette
        // state, which is what the screenshot diff golden captures.
        const smatchet::cmd::PaletteOpenDecision paletteOpen =
            smatchet::cmd::ConsumePaletteOpenLatch(g_ui.requestCommandPaletteOpen, g_ui.requestCommandPaletteFilter);
        if (paletteOpen.Open) {
            commandPalette_.Open();
            if (!paletteOpen.Filter.empty()) {
                commandPalette_.SetFilterText(paletteOpen.Filter.c_str());
            }
        }
        // Borrow the live keybinding table so palette rows surface their bound combo.
        commandPalette_.SetKeybindings(&d.cfg.Keybindings.Bindings);
        commandPalette_.Draw(app);
    }

    // "Log a Bug" modal. Its opener is now the rebindable app.bug_report.open
    // binding, default hotkey Ctrl+Shift+B, dispatched up in dispatchKeybindings.
    // This block only renders the modal once showBugReport latches, and is drawn
    // unconditionally so it still works while the active backend is unreachable.
    SmatchetBugReportUi_Draw(app, app, g_ui);

    // Quick-create issue popup — opener is the rebindable issue.quick_create.open
    // binding (default Ctrl+Shift+T). Drawn unconditionally so its in-flight create
    // future is polled (and toasted) even after the window closes.
    SmatchetQuickCreateIssueUi_Draw(app, g_ui);

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
            app.SetFieldCatalog({}, {}, {}, std::string());
            app.SetAvailableUsers({});
            d.fieldCatalogWarning.clear();
            d.fieldCatalogFetchStarted = false;
            d.fieldCatalogLoading = false;
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
    smatchet::cmd::RegisterPaneCommands(app, app, d);
    {
        SMATCHET_UI_PERF_SCOPE("TickTrackerConnectivityMonitor");
        app.TickTrackerConnectivityMonitor(g_ui.cfg);
    }
    // Per-pane ticket-change monitor (ticket-change-monitor plan, S1c-2). Hosted right after
    // the connectivity tick so it reads the freshest reachability and the same g_ui.cfg
    // (monitor enable + interval). Single per-frame site — TickAllContexts is called from two
    // hosts, so the change poll is NOT nested inside it.
    app.TickChangeMonitors(g_ui.cfg);
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
    // (Ctrl+Alt+D dock-debug toggle migrated to the rebindable app.dock_debug.toggle binding,
    // dispatched in dispatchKeybindings.)
    // Status bar — must be drawn before dockspace/other windows (viewport side-bar reservation).
    if (d.cfg.ShowStatusBar && !d.cfg.ZenMode) {
        DrawStatusBar(app, d);
    }

    // (F11 full-screen toggle migrated to the rebindable app.fullscreen.toggle binding
    // — standalone-only command, dispatched in dispatchKeybindings.)

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

#if defined(SMATCHET_BUILD_UI_TESTS)
// Test seam: a bucket-E test that programmatically rebinds a hotkey needs to trigger the
// dispatch-cache rebuild (MarkKeybindingsDirty), which no other accessor reaches on the live
// SmatchetUI. The shim sets a process-global REQUEST FLAG rather than caching a SmatchetUI* — the
// live instance consumes it at the top of its next dispatchKeybindings and marks itself dirty, so
// there is no cached instance pointer to dangle if the UI object is torn down between calls. This is
// the rebind→dirty→rebuild→fire seam that was the keybindings-editor residue; compiled out entirely
// in ship builds (zero cost).
namespace {
std::atomic<bool> g_uiTestKeybindingsDirtyRequest{false};
} // namespace
extern "C" void SmatchetUiTestMarkKeybindingsDirty() {
    g_uiTestKeybindingsDirtyRequest.store(true, std::memory_order_release);
}
#endif

// Rebuilds keybindingCache_ from cfg.Keybindings: parses each enabled binding's hotkey
// string once, drops disabled / unparseable entries, and keeps ArgsJson as text (parsed
// lazily at dispatch — only when a hotkey actually fires). Cleared + rebuilt whenever
// keybindingCacheDirty_ is set: once after config load (drawInitConfigOnce) and on any
// future editor save. Runs on the UI thread.
void SmatchetUI::rebuildKeybindingCache(UiDrawSession& d) {
    keybindingCache_.clear();
    keybindingCache_.reserve(d.cfg.Keybindings.Bindings.size());
    for (size_t i = 0; i < d.cfg.Keybindings.Bindings.size(); ++i) {
        const Keybinding& b = d.cfg.Keybindings.Bindings[i];
        if (!b.Enabled) {
            continue;
        }
        smatchet::ui::ImGuiBugHotkey hk;
        if (!smatchet::ui::ParseImGuiHotkey(b.Hotkey, hk)) {
            LOG_WARN("Keybindings: skipping unparseable hotkey \"%s\" for command \"%s\".", b.Hotkey.c_str(),
                     b.CommandId.c_str());
            continue;
        }
        ParsedKeybinding pk;
        pk.hk = hk;
        pk.commandId = b.CommandId;
        pk.argsJson = b.ArgsJson;
        keybindingCache_.push_back(std::move(pk));
    }
    keybindingCacheDirty_ = false;
}

// Per-frame rebindable keyboard-shortcut dispatch. Replaces the former hardcoded
// handleViewKeyboardShortcuts / handlePanelVisibilityShortcuts / handleViewRevealShortcuts
// plus the inline dock-debug (Ctrl+Alt+D), F11, palette-toggle (Ctrl+Shift+P), and
// bug-report polls. Lazily rebuilds the parse cache, then matches each binding against the
// current ImGui input and routes a match through the command registry. The
// ui.command_palette pseudo-binding is handled inline so it can self-gate on
// BackendHasBeenReachable and drive the palette open/close directly (the palette is a UI
// widget, not a registry command). Runs on the UI thread, before any sub-window draws.
void SmatchetUI::dispatchKeybindings(AppController& app, UiDrawSession& d) {
#if defined(SMATCHET_BUILD_UI_TESTS)
    // Consume a pending rebuild request from the keybindings rebind test seam (flag, not a cached
    // instance pointer — nothing to dangle). Runs before the dirty-check so the rebuild lands this
    // same frame.
    if (g_uiTestKeybindingsDirtyRequest.exchange(false, std::memory_order_acq_rel)) {
        MarkKeybindingsDirty();
    }
#endif
    if (keybindingCacheDirty_) {
        rebuildKeybindingCache(d);
    }
    const ImGuiIO& io = ::ImGui::GetIO();
    for (size_t i = 0; i < keybindingCache_.size(); ++i) {
        const ParsedKeybinding& pk = keybindingCache_[i];
        if (!smatchet::ui::MatchHotkey(io, pk.hk)) {
            continue;
        }
        // Pseudo-binding: the command palette is a UI widget, not a registry command.
        // Toggle it directly, gated exactly like the pre-migration Ctrl+Shift+P path.
        if (pk.commandId == "ui.command_palette") {
            if (g_ui.cfg.BackendHasBeenReachable) {
                if (commandPalette_.IsOpen()) {
                    commandPalette_.Close();
                } else {
                    commandPalette_.Open();
                }
            }
            continue;
        }
        nlohmann::json args = nlohmann::json::object();
        if (!pk.argsJson.empty()) {
            std::string parseErr;
            args = smatchet::json_safe::ParseBounded(pk.argsJson, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("Keybindings: bad args JSON for command \"%s\": %s", pk.commandId.c_str(), parseErr.c_str());
                args = nlohmann::json::object();
            }
        }
        smatchet::cmd::CommandContext ctx;
        ctx.ScenarioHost = &app;
        ctx.Threading = &app;
        ctx.Source = smatchet::cmd::CommandSource::Internal;
        const smatchet::cmd::CommandResult r = app.Commands().Dispatch(pk.commandId, args, ctx);
        if (!r.Ok) {
            LOG_WARN("Keybindings: command \"%s\" failed: %s", pk.commandId.c_str(), r.Error.Message.c_str());
        }
    }
}

// Drives the shared "Set shortcut…" quick-bind modal for the toolbar + command-palette
// right-click entry points. Each owner (toolbar_, commandPalette_) latches a one-shot
// request id we poll-and-clear here; a non-empty id opens the modal seeded with the
// command's registry Summary as the human label. A committed rebind marks the dispatch
// cache dirty + arms the debounced ConfigManager::Save via the config-save worker.
// Polled late in Draw (after both owners drew this frame) so a same-frame request opens
// same-frame. This TU's `#define ImGui` does not reach the popup — it draws via the
// shared smatchet::ui widget, which uses raw ImGui.
void SmatchetUI::drawQuickBindPopup(AppController& app, UiDrawSession& d) {
    // argsJson defaults to "{}"; the toolbar overwrites it with the clicked button's real args
    // so a parametrized button binds its (commandId, args) identity, not the default variant.
    // Command-palette rows are bare commands, so that path keeps the "{}" default.
    std::string argsJson = "{}";
    std::string cmdId = toolbar_.TakeQuickBindRequest(argsJson);
    if (cmdId.empty()) {
        cmdId = commandPalette_.TakeQuickBindRequest();
    }
    if (!cmdId.empty()) {
        std::string label = cmdId;
        const smatchet::cmd::Command* c = app.Commands().FindLocked(cmdId);
        if (c != nullptr && !c->Summary.empty()) {
            label = c->Summary;
        }
        quickBind_.Open(cmdId, label, argsJson);
    }
    if (quickBind_.Draw(d.cfg)) {
        MarkKeybindingsDirty();
        smatchet::config_save::EnqueueTrackerConfig(d.cfg);
    }
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
        SMATCHET_UI_PERF_SCOPE("SmatchetUserInfoUi::DrawWindow");
        // Bind once: the User Info popup TU stays decoupled from view plumbing.
        if (!g_ui.onUserInfoAddToQuery) {
            AppController* appPtr = &app;
            g_ui.onUserInfoAddToQuery = [this, appPtr](const std::string& paneId, const std::string& issueKey) {
                userInfoAddToQuery(*appPtr, g_ui, paneId, issueKey);
            };
        }
        const bool wantUserInfoFocus = g_ui.requestUserInfoFocus;
        if (g_ui.showUserInfo) {
            prepareTopLevelWindow(g_ui, "user_info", 720.0f, 560.0f, wantUserInfoFocus);
        }
        // Always called (even hidden) so the close-edge cleanup + future drain run.
        userInfoUi_.DrawWindow(app, g_ui, &g_ui.showUserInfo);
        if (wantUserInfoFocus) {
            g_ui.requestUserInfoFocus = false;
        }
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

// "Add to view query" from the User Info window's issue-key context menu: replace the
// source pane's view query with a single-key lookup and re-sync. Falls back to the
// focused pane when the source pane was closed since the window opened.
void SmatchetUI::userInfoAddToQuery(AppController& app, UiDrawSession& d, const std::string& sourcePaneId,
                                    const std::string& issueKey) {
    if (issueKey.empty()) {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.user_info", "User Info"),
                                              SmatchetLocalization::T("userinfo.no_issue_key", "No issue key to add."),
                                              ToastType::Warning);
        return;
    }
    GridPane* target = FindGridPaneById(d.gridPanes, sourcePaneId);
    if (!target) {
        target = FindGridPaneById(d.gridPanes, d.focusedPaneId);
    }
    if (!target) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.user_info", "User Info"),
            SmatchetLocalization::T("userinfo.source_pane_gone", "Source pane is gone; cannot update its view query."),
            ToastType::Warning);
        return;
    }
    // Per-backend key clause: Plane filter syntax vs Jira JQL.
    const std::string query = (target->backendKey == "plane") ? ("key:" + issueKey) : ("key = \"" + issueKey + "\"");
    // Shared apply path (slice 2b) — same core the global omnibar drives. Each variant maps
    // to this command's own User-Info toast copy; UpdateFailed stays a silent no-op to match
    // the original behaviour (ViewState.UpdateActive false → no toast).
    switch (applyQueryToPaneView(app, d, *target, query)) {
    case ApplyQueryResult::Ok:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.user_info", "User Info"),
            SmatchetLocalization::Format("userinfo.query_set", "View query set to %s.", issueKey.c_str()),
            ToastType::Success);
        break;
    case ApplyQueryResult::ViewUnavailable:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.user_info", "User Info"),
            SmatchetLocalization::T("userinfo.view_unavailable", "Target view is unavailable; cannot update query."),
            ToastType::Warning);
        break;
    case ApplyQueryResult::UpdateFailed:
        break;
    }
}

// Mode-independent floating overlays — toasts + the app-update modal. Drawn by BOTH the
// desktop path (head of drawSecondaryWindowsTail) and the mobile fork, so neither mode
// loses transient notifications or the update prompt. Each owns its own ImGui scope.
void SmatchetUI::drawGlobalOverlays(AppController& app, UiDrawSession& d) {
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetToastManager::Render");
        SmatchetToastManager::Instance().Render();
    }
    DrawAppUpdateModal(app, d);
    // Mode-independent home for the layout-reset confirm (latched by the Appearance menu
    // items): if it were submitted only from the menu-bar path, flipping Zen mode (or the
    // mobile fork) mid-confirm would leave an unsubmitted modal in ImGui's popup stack —
    // invisible, yet blocking all mouse input via GetTopMostPopupModal.
    drawLayoutResetConfirmModal(d);
}

// Tail half of drawSecondaryWindows: toasts, update modal, audit, AI assistant, watchers/votes
// list windows, MCP server, log window, FPS overlay. Split out for function-size compliance.
void SmatchetUI::drawSecondaryWindowsTail(AppController& app, UiDrawSession& d) {
    drawGlobalOverlays(app, d);
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
    // Notification Center. A clicked transient toast raises the manager's open-center request,
    // consumed here on the UI thread so the window opens from any Push surface, then drawn below.
    if (SmatchetToastManager::Instance().ConsumeOpenCenterRequest()) {
        d.showNotificationCenterWindow = true;
        d.requestNotificationCenterFocus = true;
    }
    if (d.showNotificationCenterWindow) {
        SMATCHET_UI_PERF_SCOPE("SmatchetDrawNotificationCenterWindow");
        SmatchetDrawNotificationCenterWindow(d);
    }
    if (g_ui.showPerformance) {
        g_perfUi.DrawFpsOverlay();
    }
}

// Dock-node debug overlay — toggled by the rebindable app.dock_debug.toggle keybinding
// (default Ctrl+Alt+D). Owns its own ::ImGui::Begin/End pair.
// LOG_DEBUG throttle counter hoisted to drawBodyState_.
void SmatchetUI::drawDockDebugOverlay(UiDrawSession& d) {
    if (!d.showDockDebug) {
        return;
    }
    const ImGuiWindowFlags kDbgFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    ::ImGui::Begin("##DockDebug", nullptr, kDbgFlags);
    // Resolve the live hotkey bound to app.dock_debug.toggle so the hint stays correct
    // after a rebind (default Ctrl+Alt+D); show no key if the user disabled the binding.
    std::string dbgHotkey;
    for (const Keybinding& b : d.cfg.Keybindings.Bindings) {
        if (b.Enabled && b.CommandId == "app.dock_debug.toggle") {
            dbgHotkey = b.Hotkey;
            break;
        }
    }
    if (dbgHotkey.empty()) {
        ::ImGui::TextDisabled("Dock Node Debug");
    } else {
        ::ImGui::TextDisabled("Dock Node Debug (%s to hide)", dbgHotkey.c_str());
    }
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
    // Drain a latched layout reset HERE, at end-of-frame. SmatchetUI_ResetLayoutToDefault is
    // always invoked mid-frame (menu / command) and only sets the latch; applying the heavy
    // dock work (LoadIniSettingsFromMemory + force-redock arming) inline corrupts the live tree.
    // This is the proven-safe timing — the queued node rebuild lands on the next NewFrame and
    // the force-redock pass then runs against a freshly-built tree. Runs before the
    // layoutForceDefaultsFrames countdown below so the freshly-armed 8 frames take effect.
    SmatchetUI_ApplyDeferredLayoutReset(d);
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
void SmatchetUI::drawAiAssistantPanel(AppController& app, UiDrawSession& d, bool embedded) {
    // Free function lives in SmatchetAiAssistantUi.cpp; this member exists to keep
    // SmatchetUI.h's private-method contract uniform with the other window drawers.
    // Phase C: forward the active view definition so the panel's auto-context builder
    // can populate the ActiveView block. embedded forwards the mobile page-body mode.
    SmatchetDrawAiAssistantPanel(app, d, ViewState.GetActiveView(), embedded);
}
#endif

void SmatchetUI::drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d) {
    const auto startCatalogFetch = [&](const TrackerConfig& fetchCfg) {
        if (d.fieldCatalogLoading) {
            return;
        }
        if (!app.BackendShared()) {
            return;
        }
        d.fieldCatalogLoading = true;
        d.fieldCatalogFetchStarted = true;
        const ViewDefinition* activeView = ViewState.GetActiveView();
        const std::string jql = activeView ? activeView->Jql : fetchCfg.JqlQuery;
        d.fieldCatalogFuture = StartFieldCatalogFetchAsync(app, fetchCfg, jql);
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
            } else if (result.Error.find("Tracker backend is not initialized") != std::string::npos) {
                d.fieldCatalogLoading = false;
                d.fieldCatalogFetchStarted = false;
                d.triggerCatalogRefetch = true;
            } else {
                app.SetFieldCatalog({}, {},
                                    result.Error.empty() ? std::string("Failed to fetch field catalog.") : result.Error,
                                    result.ErrorTransient);
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
            // Initial refresh already covers a same-frame connectivity-recovery latch; skip the
            // follow-up resync on the next frame (would duplicate SyncWithBackend / toasts).
            d.connectivityRecoveryTicketResyncPending = false;
            const bool suppressKeyMatches =
                d.suppressNextBackendSwitchInitialSyncKey == ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
            const bool suppressThisKick = d.suppressNextBackendSwitchInitialSync && suppressKeyMatches;
            // Consume-once either way — a stale latch (key mismatch, e.g. a same-frame
            // Preferences backend switch) must not survive to swallow a later sync.
            d.suppressNextBackendSwitchInitialSync = false;
            d.suppressNextBackendSwitchInitialSyncKey.clear();
            if (suppressThisKick) {
                d.initialTicketSyncStarted = true;
                d.appliedInitialView = true;
            } else if (d.gridPanesLoaded && !d.gridPanes.empty()) {
                const GridPane& focused = d.focusedPane();
                if (!app.IsPaneSyncLive(focused.id)) {
                    TrackerConfig paneCfg = d.cfg;
                    if (!focused.backendKey.empty()) {
                        paneCfg.TrackerType = focused.backendKey;
                    }
                    app.EnsurePaneLiveSyncStarted(focused.id, paneCfg, focused.viewId);
                }
                d.initialTicketSyncStarted = true;
                d.appliedInitialView = true;
            }
            // else: panes not loaded yet — drawGridPaneWindows kicks the focused pane same
            // frame (EnsurePanesLoaded + SetFocusedPane) and marks initialTicketSyncStarted.
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
