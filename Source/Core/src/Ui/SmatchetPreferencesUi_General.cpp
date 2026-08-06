// General category of the Preferences window: Updates, Language & region,
// Storage, and Local database. The Updates / date-format blocks came from the
// old Appearance tab and the storage / recreate-DB blocks from the old
// "Local data" tab; both tabs are gone as top-level entries.

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
// Not a new dependency: the Updates / Storage / Local-database bodies moved here verbatim from
// SmatchetPreferencesUi_Local.cpp, which dropped its own AppController.h include in the same
// change, so tree-wide fan-in is net-unchanged. They call app.CheckForAppUpdate / RequestAppQuit /
// GetResolvedLocalCacheDbPath / RecreateLocalCacheDatabase / SyncWithBackend — no narrower header.
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=include relocated from SmatchetPreferencesUi_Local.cpp, which
// dropped its own; net fan-in unchanged; owner=prefs-ia; revisit=2026-12-01)
#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetHelpMarker.h"
#include "Ui/SmatchetDestructiveButton.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <future>
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

// Updates section: auto-check + prerelease toggles, manual check, skip-version.
void DrawGeneralUpdatesSection(AppController& app, UiDrawSession& d) {
    if (d.prefsFilter.ShowSetting("general.updates.check_enabled")) {
        if (ImGui::Checkbox("Check for updates automatically", &d.cfg.UpdateCheckEnabled)) {
            MarkPrefsDirty(d);
        }
    }
    if (d.prefsFilter.ShowSetting("general.updates.include_prerelease")) {
        if (ImGui::Checkbox("Include prerelease builds", &d.cfg.UpdateIncludePrerelease)) {
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("When enabled, startup and manual checks can target prerelease GitHub releases too.");
    }
    if (d.prefsFilter.ShowSetting("general.updates.check_now")) {
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
    }
    // Status readouts, not settings: they own no descriptor row, so they stay out
    // of the filtered view rather than padding a section the query narrowed down.
    if (!d.prefsFilter.Active()) {
        if (!d.cfg.UpdateSkipVersion.empty()) {
            ImGui::TextDisabled("Skipped version: %s", d.cfg.UpdateSkipVersion.c_str());
            if (ImGui::SmallButton("Clear skipped version")) {
                d.cfg.UpdateSkipVersion.clear();
                MarkPrefsDirty(d);
            }
        }
        ImGui::TextDisabled("GitHub release repo: %s", d.cfg.UpdateGithubRepo.c_str());
    }
}

// Language & region: UI language combo (was next to the font combo on the old
// Appearance tab) plus the date-format style and its compact-relative threshold.
void DrawGeneralLanguageRegionSection(UiDrawSession& d) {
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
    if (d.prefsFilter.ShowSetting("general.language_region.language")) {
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
        ImGui::Spacing();
    }

    const char* dateFormats[] = {SmatchetLocalization::T("prefs.date_relative_compact", "Relative / Compact"),
                                 SmatchetLocalization::T("prefs.date_always_relative", "Always Relative"),
                                 SmatchetLocalization::T("prefs.date_absolute_iso", "Absolute ISO"),
                                 SmatchetLocalization::T("prefs.date_absolute_friendly", "Absolute Friendly")};
    int currentDateFormatIdx = SmatchetPreferencesUiDetail::DateFormatOptionToIndex(d.cfg.DateFormatOption);
    if (d.prefsFilter.ShowSetting("general.language_region.date_format")) {
        if (ImGui::Combo("Date Format Style", &currentDateFormatIdx, dateFormats, IM_ARRAYSIZE(dateFormats))) {
            d.cfg.DateFormatOption = SmatchetPreferencesUiDetail::DateFormatIndexToOption(currentDateFormatIdx);
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("How date values render across the UI.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.appearance.date_format.help",
                                   "Select how date and datetime values are rendered in the grids and UI "
                                   "panels.");
    }

    // ConditionalDraw row: the threshold only exists for the compact style, so the
    // coverage guard tolerates it never being observed.
    if (currentDateFormatIdx == 0 && d.prefsFilter.ShowSetting("general.language_region.date_compact_threshold")) {
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

// Storage: Portable/Shared storage-mode combo and the restart prompt.
void DrawGeneralStorageSection(AppController& app, UiDrawSession& d) {
    // `app` (RequestAppQuit) is only reached on the non-Unreal restart path below; in the
    // SMATCHET_EMBEDDED_IN_UNREAL build that block is compiled out, leaving it unreferenced.
    (void)app;
    if (!d.prefsFilter.ShowSetting("general.storage.mode")) {
        return;
    }
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

// Local database: intro blurb, resolved cache path, the "Recreate database..."
// button and its confirm modal (modal stays whole with its opener).
void DrawGeneralLocalDatabaseSection(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    if (!d.prefsFilter.ShowSetting("general.local_database.recreate")) {
        return;
    }
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

} // namespace

void DrawGeneralPreferencesTab(SmatchetUI& ui, AppController& app, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.updates", [&] { DrawGeneralUpdatesSection(app, d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.language_region",
                                              [&] { DrawGeneralLanguageRegionSection(d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.storage", [&] { DrawGeneralStorageSection(app, d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.local_database",
                                              [&] { DrawGeneralLocalDatabaseSection(ui, app, d); });
}
