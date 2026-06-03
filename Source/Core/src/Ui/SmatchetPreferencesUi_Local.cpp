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
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstring>
#include <string>

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

// "Local data" tab body — owns its own tab-item begin/end pair (the end runs only when
// the begin returned true). Split out of DrawLocalAndAppearancePreferencesTabs during the
// function-size decomposition; behaviour-identical.
void DrawLocalDataTab(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    if (ImGui::BeginTabItem("Local data")) {
        ImGui::TextWrapped(
            "Stored tickets, offline create queues, and pending field edits live in a local SQLite file. "
            "Recreating it clears that data only; tracker credentials and views are not removed. A full "
            "issue refresh runs afterward.");
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
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete and recreate")) {
                std::string err;
                if (app.RecreateLocalCacheDatabase(err)) {
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
                        err.empty() ? std::string(detail) : err, ToastType::Error, 6000);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Settings storage location");
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
        ImGui::TextWrapped("Plugin default: writable files (config / views / SQLite cache / ImGui layout) live in "
                           "<UnrealProject>/Saved next to the runtime cache. Switch to Shared when the project dir "
                           "is read-only (source-controlled, network share, sandboxed runner) and Smatchet should "
                           "instead use your OS user-data folder. Change takes effect on next launch.");
#else
        ImGui::TextWrapped("Standalone default: writable files live in your OS user-data folder, shared across "
                           "exes / installs. Switch to Portable to keep all writable files next to the executable "
                           "instead — useful when running from a thumb drive or testing parallel builds. Change "
                           "takes effect on next launch.");
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
        const char* items[] = {"Portable (next to runtime files)", "Shared (OS user-data folder)"};
        static bool s_storageModeChanged = false;
        if (ImGui::Combo("Storage mode", &prefIndex, items, IM_ARRAYSIZE(items))) {
            const ConfigManager::StoragePreference chosen = (prefIndex == 0)
                                                                ? ConfigManager::StoragePreference::Portable
                                                                : ConfigManager::StoragePreference::Shared;
            std::string err;
            if (ConfigManager::SetStoragePreference(runtimeAssetDir, chosen, err)) {
                s_storageModeChanged = true;
                SmatchetToastManager::Instance().Push(
                    std::string("Storage"),
                    std::string(chosen == ConfigManager::StoragePreference::Portable
                                    ? "Storage mode set to Portable. Restart Smatchet for the new "
                                      "writable-files location to take effect."
                                    : "Storage mode set to Shared. Restart Smatchet for the new "
                                      "writable-files location to take effect."),
                    ToastType::Info, 6000);
            } else {
                SmatchetToastManager::Instance().Push(std::string("Storage"),
                                                      err.empty() ? std::string("Could not write storage-mode "
                                                                                "marker file.")
                                                                  : err,
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
                        std::string("Storage"), std::string("Could not relaunch Smatchet — exit and restart manually."),
                        ToastType::Error, 6000);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(spawns a new instance and exits this one)");
#endif
        }
        ImGui::TextDisabled("Current writable directory: %s", ConfigManager::GetUserDataDirectory().c_str());
        ImGui::TextDisabled("Marker file: %s", ConfigManager::GetStoragePreferenceFlagPath(runtimeAssetDir).c_str());

        ImGui::EndTabItem();
    }
}

// "Appearance" tab body — owns its own tab-item begin/end pair (the end runs only when
// the begin returned true). Split out of DrawLocalAndAppearancePreferencesTabs during the
// function-size decomposition; behaviour-identical.
void DrawAppearanceTab(AppController& app, UiDrawSession& d) {
    if (ImGui::BeginTabItem("Appearance")) {
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
        ImGui::SetItemTooltip(
            "Select the typography for the entire application. Rebuilds and reloads the font atlas instantly.");

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
        ImGui::SetItemTooltip(
            "Select the UI language. App-owned UI text changes immediately; tracker data is shown as-is.");

        ImGui::Spacing();
        ImGui::TextUnformatted("Grid and field text");
        ImGui::Separator();
        if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip(
            "When a value is truncated to fit the cell, or spans multiple lines, hover to read the full text in a "
            "tooltip.");
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
        ImGui::SetItemTooltip(
            "At top/bottom of the ticket grid, vertical wheel starts horizontal scrolling after this many wheel "
            "ticks. 0 routes immediately.");

        ImGui::Spacing();
        ImGui::TextUnformatted("Date Formatting");
        ImGui::Separator();
        ImGui::Spacing();

        const char* dateFormats[] = {"Relative / Compact", "Always Relative", "Absolute ISO", "Absolute Friendly"};
        int currentDateFormatIdx = SmatchetPreferencesUiDetail::DateFormatOptionToIndex(d.cfg.DateFormatOption);

        if (ImGui::Combo("Date Format Style", &currentDateFormatIdx, dateFormats, IM_ARRAYSIZE(dateFormats))) {
            d.cfg.DateFormatOption = SmatchetPreferencesUiDetail::DateFormatIndexToOption(currentDateFormatIdx);
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("Select how date and datetime values are rendered in the grids and UI panels.");

        if (currentDateFormatIdx == 0) {
            int threshold = d.cfg.DateCompactRelativeThresholdDays;
            if (ImGui::SliderInt("Compact Relative Threshold (Days)", &threshold, 1, 90, "%d days")) {
                d.cfg.DateCompactRelativeThresholdDays = threshold;
                MarkPrefsDirty(d);
            }
            ImGui::SetItemTooltip("Threshold in days where the compact view transitions from relative (e.g. -3d) "
                                  "to short absolute (e.g. May 07 '26).");
        }

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
            d.appUpdateFuture = std::async(std::launch::async, [&app, cfg = d.cfg]() {
                return app.CheckForAppUpdate(cfg.UpdateIncludePrerelease);
            });
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

        ImGui::EndTabItem();
    }
}

} // namespace

void DrawLocalAndAppearancePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    DrawLocalDataTab(ui, app, d);
    DrawAppearanceTab(app, d);
}
