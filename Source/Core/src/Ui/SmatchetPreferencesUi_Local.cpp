#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#if !defined(SMATCHET_EMBEDDED_IN_UNREAL)
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#endif

#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetUI.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetHelpMarker.h"
#include "Ui/SmatchetDestructiveButton.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "VsyncControl.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if !defined(SMATCHET_EMBEDDED_IN_UNREAL)
namespace {
std::string GetCurrentExePath() {
#if defined(_WIN32)
    char buf[4096] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
#elif defined(__linux__)
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
#elif defined(__APPLE__)
    char buf[4096] = {};
    uint32_t sz = sizeof(buf);
    return (_NSGetExecutablePath(buf, &sz) == 0) ? std::string(buf) : std::string();
#else
    return std::string();
#endif
}

bool LaunchDetachedSelf() {
    const std::string exe = GetCurrentExePath();
    if (exe.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::string cmdLine = "\"" + exe + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok =
        CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        pid_t pid2 = fork();
        if (pid2 < 0)
            _exit(127);
        if (pid2 == 0) {
            execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}
} // namespace
#endif // !SMATCHET_EMBEDDED_IN_UNREAL

namespace {

// Recreate-database section of the Local-data tab: intro blurb, resolved cache path, the
// "Recreate database..." button and its confirm modal. Extracted from DrawLocalDataTab during
// the over-100-line decomposition; behaviour-identical (the modal stays whole with its opener).
void DrawLocalDataRecreateDbSection(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    ImGui::TextUnformatted("Local SQLite cache: tickets, offline queues, pending edits.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.local_data.recreate_intro.help",
                               "Stored tickets, offline create queues, and pending field edits live in a "
                               "local SQLite file. Recreating it clears that data only; tracker credentials "
                               "and views are not removed. A full issue refresh runs afterward.");
    ImGui::Spacing();
    const std::string resolved = app.GetResolvedLocalCacheDbPath();
    if (!resolved.empty()) {
        ImGui::TextDisabled("Cache file:");
        ImGui::SameLine();
        ::ImGui::TextWrapped("%s", resolved.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Recreate database...")) {
        ImGui::OpenPopup("Delete local database?###RecreateSqliteDbConfirm");
    }
    ImGui::SetItemTooltip("Permanently delete the local cache file and start with an empty database.");

    if (ImGui::BeginPopupModal("Delete local database?###RecreateSqliteDbConfirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "This removes cached issues and any queued offline writes stored on this machine. It does not "
            "delete anything on the tracker. Continue?");
        ImGui::Separator();
        // P2-M16: action-first order + the shared destructive styling (this modal was
        // the one Cancel-first outlier).
        SmatchetPushDestructiveButtonColors();
        const bool deleteClicked = ImGui::Button("Delete and recreate");
        SmatchetPopDestructiveButtonColors();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        if (deleteClicked) {
            const VoidResult recreated = app.RecreateLocalCacheDatabase();
            if (recreated.has_value()) {
                SmatchetToastManager::Instance().Push(
                    std::string(SmatchetLocalization::T("toast.success", "Success")),
                    std::string(SmatchetLocalization::T("toast.local_db_recreated",
                                                        "Local database recreated; refreshing issues.")),
                    ToastType::Success, 4000);
                app.SyncWithBackend(&d.cfg, &ui.GetViewsStore());
                ImGui::CloseCurrentPopup();
            } else {
                const char* detail = SmatchetLocalization::T("toast.local_db_recreate_failed_detail",
                                                             "Could not recreate the local database.");
                SmatchetToastManager::Instance().Push(
                    std::string(SmatchetLocalization::T("toast.local_db_error_title", "Local database")),
                    recreated.error().empty() ? std::string(detail) : recreated.error(), ToastType::Error, 6000);
            }
        }
        ImGui::EndPopup();
    }
}

// Settings-storage-location section of the Local-data tab: Portable/Shared storage-mode combo and
// the restart prompt. Extracted from DrawLocalDataTab during the over-100-line decomposition.
void DrawLocalDataStorageSection(AppController& app) {
    // `app` (RequestAppQuit) is only reached on the non-Unreal restart path below; in the
    // SMATCHET_EMBEDDED_IN_UNREAL build that block is compiled out, leaving it unreferenced.
    (void)app;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Settings storage location");
    ImGui::SameLine();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    SmatchetHelpMarker::Render("prefs.local_data.storage_unreal.help",
                               "Plugin default: writable files (config / views / SQLite cache / ImGui "
                               "layout) live in <UnrealProject>/Saved next to the runtime cache. Switch to "
                               "Shared when the project dir is read-only (source-controlled, network share, "
                               "sandboxed runner) and Smatchet should instead use your OS user-data folder. "
                               "Change takes effect on next launch.");
#else
    SmatchetHelpMarker::Render("prefs.local_data.storage_standalone.help",
                               "Standalone default: writable files live in your OS user-data folder, shared "
                               "across exes / installs. Switch to Portable to keep all writable files next "
                               "to the executable instead — useful when running from a thumb drive or "
                               "testing parallel builds. Change takes effect on next launch.");
#endif
    const std::string runtimeAssetDir = ConfigManager::GetRuntimeAssetDirectory();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    constexpr ConfigManager::StoragePreference kDefaultPref = ConfigManager::StoragePreference::Portable;
#else
    constexpr ConfigManager::StoragePreference kDefaultPref = ConfigManager::StoragePreference::Shared;
#endif
    const ConfigManager::StoragePreference currentPref =
        ConfigManager::GetStoragePreference(runtimeAssetDir, kDefaultPref);
    int prefIndex = (currentPref == ConfigManager::StoragePreference::Portable) ? 0 : 1;
    const char* items[] = {SmatchetLocalization::T("prefs.storage.portable", "Portable (next to runtime files)"),
                           SmatchetLocalization::T("prefs.storage.shared", "Shared (OS user-data folder)")};
    static bool s_storageModeChanged = false;
    if (ImGui::Combo("Storage mode", &prefIndex, items, IM_ARRAYSIZE(items))) {
        const ConfigManager::StoragePreference chosen =
            (prefIndex == 0) ? ConfigManager::StoragePreference::Portable : ConfigManager::StoragePreference::Shared;
        const VoidResult stored = ConfigManager::SetStoragePreference(runtimeAssetDir, chosen);
        if (stored.has_value()) {
            s_storageModeChanged = true;
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.storage", "Storage"),
                chosen == ConfigManager::StoragePreference::Portable
                    ? SmatchetLocalization::T("prefs.storage.set_portable",
                                              "Storage mode set to Portable. Restart Smatchet for the new "
                                              "writable-files location to take effect.")
                    : SmatchetLocalization::T("prefs.storage.set_shared",
                                              "Storage mode set to Shared. Restart Smatchet for the new "
                                              "writable-files location to take effect."),
                ToastType::Info, 6000);
        } else {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.storage", "Storage"),
                stored.error().empty()
                    ? std::string(SmatchetLocalization::T("prefs.storage.marker_write_failed",
                                                          "Could not write storage-mode marker file."))
                    : stored.error(),
                ToastType::Error, 6000);
        }
    }
    if (s_storageModeChanged) {
        ImGui::Spacing();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
        ImGui::TextWrapped("Restart Unreal Editor for the storage-mode change to take effect.");
#else
        if (ImGui::Button("Restart Smatchet now")) {
            if (LaunchDetachedSelf()) {
                s_storageModeChanged = false;
                app.RequestAppQuit();
            } else {
                SmatchetToastManager::Instance().Push(
                    SmatchetLocalization::T("toast.storage", "Storage"),
                    SmatchetLocalization::T("prefs.storage.relaunch_failed",
                                            "Could not relaunch Smatchet — exit and restart manually."),
                    ToastType::Error, 6000);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(spawns a new instance and exits this one)");
#endif
    }
    ImGui::TextDisabled("Current writable directory: %s", ConfigManager::GetUserDataDirectory().c_str());
    ImGui::TextDisabled("Marker file: %s", ConfigManager::GetStoragePreferenceFlagPath(runtimeAssetDir).c_str());
}


// Typography section of the Appearance tab: application font + UI language combos.
// Extracted from DrawAppearanceTab during the over-100-line decomposition; behaviour-identical.
void DrawAppearanceTypographySection(UiDrawSession& d) {
    ImGui::TextUnformatted("Application Typography");
    ImGui::Separator();
    ImGui::Spacing();

    const char* fonts[] = {
        "Segoe UI",       "Proggy (Clean/Default)", "Consolas",     "Arial",  "Courier New", "Georgia",
        "Lucida Console", "Microsoft Sans Serif",   "Trebuchet MS", "Verdana"};
    int currentFontIdx = 0;
    for (int i = 0; i < 10; ++i) {
        if (d.cfg.SelectedFontName == fonts[i]) {
            currentFontIdx = i;
            break;
        }
    }
    if (ImGui::Combo("Application Font", &currentFontIdx, fonts, 10)) {
        d.cfg.SelectedFontName = fonts[currentFontIdx];
        MarkPrefsDirty(d);
        SmatchetRequestFontReload(d.cfg.SelectedFontName, static_cast<float>(d.cfg.FontSizePt));
    }
    ImGui::SetItemTooltip("Application-wide font; applies instantly.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.font.help",
                               "Select the typography for the entire application. Rebuilds and reloads the "
                               "font atlas instantly.");

    const auto& languages = SmatchetLocalization::AvailableLanguages();
    int currentLanguageIdx = 0;
    for (int i = 0; i < static_cast<int>(languages.size()); ++i) {
        if (d.cfg.UiLanguage == languages[static_cast<size_t>(i)].Code) {
            currentLanguageIdx = i;
            break;
        }
    }
    const char* languageItems[] = {SmatchetLocalization::T("language.en_us", "English"),
                                   SmatchetLocalization::T("language.fr_native", u8"Français")};
    if (ImGui::Combo("Language", &currentLanguageIdx, languageItems, IM_ARRAYSIZE(languageItems))) {
        if (currentLanguageIdx >= 0 && currentLanguageIdx < static_cast<int>(languages.size())) {
            d.cfg.UiLanguage = languages[static_cast<size_t>(currentLanguageIdx)].Code;
            MarkPrefsDirty(d);
            SmatchetLocalization::SetLanguage(d.cfg.UiLanguage);
        }
    }
    ImGui::SetItemTooltip("UI language; applies immediately.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.language.help",
                               "Select the UI language. App-owned UI text changes immediately; tracker data "
                               "is shown as-is.");
}

// Grid-and-field-text section of the Appearance tab: overflow tooltips + wheel-swallow ticks.
// Extracted from DrawAppearanceTab during the over-100-line decomposition; behaviour-identical.
void DrawAppearanceGridTextSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Grid and field text");
    ImGui::Separator();
    if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Hover truncated cells to read the full text.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.overflow_tooltips.help",
                               "When a value is truncated to fit the cell, or spans multiple lines, hover "
                               "to read the full text in a tooltip.");
    int gridWheelSwallowTicks = d.cfg.GridEndWheelSwallowsBeforeHorizontal;
    if (ImGui::InputInt("Wheel ticks before horizontal scroll", &gridWheelSwallowTicks)) {
        if (gridWheelSwallowTicks < 0) {
            gridWheelSwallowTicks = 0;
        }
        if (gridWheelSwallowTicks > 32) {
            gridWheelSwallowTicks = 32;
        }
        d.cfg.GridEndWheelSwallowsBeforeHorizontal = gridWheelSwallowTicks;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Wheel ticks swallowed at the grid edge.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.wheel_swallow.help",
                               "At top/bottom of the ticket grid, vertical wheel starts horizontal "
                               "scrolling after this many wheel ticks. 0 routes immediately.");
}

// Date-formatting section of the Appearance tab: format-style combo + compact threshold slider.
// Extracted from DrawAppearanceTab during the over-100-line decomposition; behaviour-identical.
void DrawAppearanceDateSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Date Formatting");
    ImGui::Separator();
    ImGui::Spacing();

    const char* dateFormats[] = {SmatchetLocalization::T("prefs.date_relative_compact", "Relative / Compact"),
                                 SmatchetLocalization::T("prefs.date_always_relative", "Always Relative"),
                                 SmatchetLocalization::T("prefs.date_absolute_iso", "Absolute ISO"),
                                 SmatchetLocalization::T("prefs.date_absolute_friendly", "Absolute Friendly")};
    int currentDateFormatIdx = SmatchetPreferencesUiDetail::DateFormatOptionToIndex(d.cfg.DateFormatOption);

    if (ImGui::Combo("Date Format Style", &currentDateFormatIdx, dateFormats, IM_ARRAYSIZE(dateFormats))) {
        d.cfg.DateFormatOption = SmatchetPreferencesUiDetail::DateFormatIndexToOption(currentDateFormatIdx);
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("How date values render across the UI.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.date_format.help",
                               "Select how date and datetime values are rendered in the grids and UI "
                               "panels.");

    if (currentDateFormatIdx == 0) {
        int threshold = d.cfg.DateCompactRelativeThresholdDays;
        if (ImGui::SliderInt("Compact Relative Threshold (Days)", &threshold, 1, 90, "%d days")) {
            d.cfg.DateCompactRelativeThresholdDays = threshold;
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("Days before compact dates switch to absolute.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.appearance.date_threshold.help",
                                   "Threshold in days where the compact view transitions from relative "
                                   "(e.g. -3d) to short absolute (e.g. May 07 '26).");
    }
}

// "Display" block — render/swapchain toggles. The vsync checkbox applies live
// via the smatchet::vsync hub (render loop picks it up next frame) and persists
// with the prefs Save like the sibling Appearance toggles.
void DrawAppearanceDisplaySection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Display");
    ImGui::Separator();
    if (ImGui::Checkbox("Enable vsync", &d.cfg.VsyncEnabled)) {
        smatchet::vsync::SetEnabled(d.cfg.VsyncEnabled);
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Sync rendering with the monitor refresh rate.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.appearance.vsync.help",
                               "Synchronize rendering with the monitor refresh rate. Disabling uncaps the "
                               "frame rate (higher CPU/GPU usage).");
    // P2-M6: the confirm modal's "Don't ask again" needs a discoverable way back on.
    bool askBeforeLayoutReset = !d.cfg.SkipLayoutResetConfirm;
    if (ImGui::Checkbox("Ask before layout-resetting changes", &askBeforeLayoutReset)) {
        d.cfg.SkipLayoutResetConfirm = !askBeforeLayoutReset;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Confirm before panel moves and Reset Layout replace your window arrangement.");
}

// Updates section of the Appearance tab: auto-check + prerelease toggles, manual check, skip-version.
// Extracted from DrawAppearanceTab during the over-100-line decomposition; behaviour-identical.
void DrawAppearanceUpdatesSection(AppController& app, UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Updates");
    ImGui::Separator();
    if (ImGui::Checkbox("Check for updates automatically", &d.cfg.UpdateCheckEnabled)) {
        MarkPrefsDirty(d);
    }
    if (ImGui::Checkbox("Include prerelease builds", &d.cfg.UpdateIncludePrerelease)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("When enabled, startup and manual checks can target prerelease GitHub releases too.");
    if (ImGui::Button("Check for Updates Now")) {
        d.appUpdateStartupCheckStarted = true;
        d.appUpdateActionStatus.clear();
        d.appUpdateCheckManual = true;
        d.appUpdateCheckInFlight = true;
        d.appUpdateFuture = std::async(
            std::launch::async, [&app, cfg = d.cfg]() { return app.CheckForAppUpdate(cfg.UpdateIncludePrerelease); });
    }
    if (d.appUpdateCheckInFlight) {
        ImGui::SameLine();
        ImGui::TextDisabled("(checking...)");
    }
    if (!d.cfg.UpdateSkipVersion.empty()) {
        ImGui::TextDisabled("Skipped version: %s", d.cfg.UpdateSkipVersion.c_str());
        if (ImGui::SmallButton("Clear skipped version")) {
            d.cfg.UpdateSkipVersion.clear();
            MarkPrefsDirty(d);
        }
    }
    ImGui::TextDisabled("GitHub release repo: %s", d.cfg.UpdateGithubRepo.c_str());
}

// Ticket-change monitor section (docs/plans/ticket-change-monitor.md). Enable toggle +
// poll-interval slider (greyed when the monitor is off). Both persist via MarkPrefsDirty like
// the sibling Appearance toggles; the interval matches the [30, 3600] s load-clamp band.
void DrawAppearanceNotificationsSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Ticket change monitor");
    ImGui::Separator();
    if (ImGui::Checkbox("Notify me when tracked issues change", &d.cfg.TicketChangeMonitorEnabled)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Periodically poll the backend for changes to the open panes' tickets "
                          "and raise an in-app toast.");
    ImGui::BeginDisabled(!d.cfg.TicketChangeMonitorEnabled);
    int interval = d.cfg.TicketChangeMonitorIntervalSec;
    if (ImGui::SliderInt("Check interval (seconds)", &interval, 30, 3600, "%d s")) {
        d.cfg.TicketChangeMonitorIntervalSec = interval;
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("How often to poll for changes. Lower = faster notice, more requests.");
    ImGui::EndDisabled();
}

// Layout-mode section of the Appearance tab: Desktop / Mobile / Auto selector.
// Auto resolves by viewport width per frame; Desktop/Mobile pin the mode. Saves
// immediately via MarkPrefsDirty like the sibling Appearance toggles. The combo
// index maps 1:1 onto the UiMode enum order (Desktop=0/Mobile=1/Auto=2).
void DrawAppearanceUiModeSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Layout mode");
    ImGui::Separator();
    const char* modes[] = {SmatchetLocalization::T("prefs.uimode.desktop", "Desktop"),
                           SmatchetLocalization::T("prefs.uimode.mobile", "Mobile"),
                           SmatchetLocalization::T("prefs.uimode.auto", "Auto (by width)")};
    int currentMode = static_cast<int>(d.cfg.UiMode);
    if (ImGui::Combo("UI mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
        if (currentMode >= 0 && currentMode <= 2) {
            d.cfg.UiMode = static_cast<UiMode>(currentMode);
            MarkPrefsDirty(d);
        }
    }
    ImGui::SetItemTooltip("Desktop = docking workspace. Mobile = single-column shell with top bar + bottom nav. "
                          "Auto switches by window width.");
}

// Mobile-shell customization (Appearance tab): touch density, bottom-nav page
// order + show/hide, and the home page. Only meaningful in Mobile/Auto, but shown
// always so the layout is configurable from a desktop session. Every mutation
// re-runs ConfigManager::SanitizeMobileNav (drop-unknown / dedup / >=1-visible /
// home-present guards) then MarkPrefsDirty for the debounced save.
void DrawAppearanceMobileSection(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Mobile layout");
    ImGui::Separator();

    // Touch density radio.
    int density = static_cast<int>(d.cfg.MobileTouchDensity);
    bool densityChanged = false;
    densityChanged |= ImGui::RadioButton("Compact", &density, static_cast<int>(MobileTouchDensity::Compact));
    ImGui::SameLine();
    densityChanged |= ImGui::RadioButton("Comfortable", &density, static_cast<int>(MobileTouchDensity::Comfortable));
    if (densityChanged) {
        d.cfg.MobileTouchDensity = static_cast<MobileTouchDensity>(density);
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Comfortable enlarges hit targets and fonts; Compact trades reach for density.");

    // Bottom-nav editor: visible pages first (ordered, with up/down + hide), then
    // the remaining hidden pages as unchecked rows to re-add.
    ImGui::Spacing();
    ImGui::TextDisabled("Bottom navigation");
    static const char* const kAllPageIds[] = {"grid", "views", "log", "settings", "ai"};
    std::vector<std::string>& nav = d.cfg.MobileNavPages;

    for (int i = 0; i < static_cast<int>(nav.size()); ++i) {
        ImGui::PushID(i);
        // Explicit hide action — a button, not a checkbox seeded to a constant. A visible
        // page only ever has one meaningful action here: drop it from the ordered nav list.
        // Re-adding a hidden page is handled by the separate unchecked-rows loop below.
        if (ImGui::SmallButton("Hide")) {
            nav.erase(nav.begin() + i);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
            std::swap(nav[static_cast<std::size_t>(i)], nav[static_cast<std::size_t>(i - 1)]);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < static_cast<int>(nav.size())) {
            std::swap(nav[static_cast<std::size_t>(i)], nav[static_cast<std::size_t>(i + 1)]);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(mobileNavPageLabel(nav[static_cast<std::size_t>(i)]).c_str());
        ImGui::PopID();
    }

    for (const char* id : kAllPageIds) {
        bool present = false;
        for (const std::string& cur : nav) {
            if (cur == id) {
                present = true;
                break;
            }
        }
        if (present) {
            continue;
        }
        ImGui::PushID(id);
        bool visible = false;
        if (ImGui::Checkbox("##vis", &visible)) {
            nav.push_back(id);
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s (hidden)", mobileNavPageLabel(id).c_str());
        ImGui::PopID();
    }

    // Home page combo (from the visible pages).
    ImGui::Spacing();
    int homeIdx = 0;
    std::vector<std::string> homeLabels;
    for (int i = 0; i < static_cast<int>(nav.size()); ++i) {
        homeLabels.push_back(mobileNavPageLabel(nav[static_cast<std::size_t>(i)]));
        if (nav[static_cast<std::size_t>(i)] == d.cfg.MobileHomePage) {
            homeIdx = i;
        }
    }
    // ImGui::Combo wants a const char* array; build it after homeLabels is fully
    // populated so the std::string storage no longer reallocates (the pointers
    // stay valid for the Combo call).
    std::vector<const char*> homeLabelPtrs;
    homeLabelPtrs.reserve(homeLabels.size());
    for (const std::string& label : homeLabels) {
        homeLabelPtrs.push_back(label.c_str());
    }
    if (!homeLabelPtrs.empty() &&
        ImGui::Combo("Home page", &homeIdx, homeLabelPtrs.data(), static_cast<int>(homeLabelPtrs.size()))) {
        if (homeIdx >= 0 && homeIdx < static_cast<int>(nav.size())) {
            d.cfg.MobileHomePage = nav[static_cast<std::size_t>(homeIdx)];
            ConfigManager::SanitizeMobileNav(d.cfg);
            MarkPrefsDirty(d);
        }
    }
    ImGui::SetItemTooltip("The page shown first when the mobile shell opens.");
}

} // namespace

void DrawLocalDataPreferencesTab(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.local_database", [&] {
        DrawLocalDataRecreateDbSection(ui, app, d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.storage", [&] {
        DrawLocalDataStorageSection(app);
    });
}

void DrawAppearancePreferencesTab(AppController& app, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.theme_font", [&] {
        DrawAppearanceTypographySection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.layout_density", [&] {
        DrawAppearanceGridTextSection(d);
        DrawAppearanceMobileSection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.language_region", [&] {
        DrawAppearanceDateSection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "appearance.display", [&] {
        DrawAppearanceDisplaySection(d);
        DrawAppearanceUiModeSection(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.updates", [&] {
        DrawAppearanceUpdatesSection(app, d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "tracker.notifications", [&] {
        DrawAppearanceNotificationsSection(d);
    });
}
