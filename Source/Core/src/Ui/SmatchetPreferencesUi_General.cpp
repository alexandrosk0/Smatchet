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
// AppController fan-in: these sections need only the update/quit (IAppMeta) and local-cache
// (IAppSync) facets.
#include "ConfigManager.h"
#include "Interfaces/IAppMeta.h"
#include "Interfaces/IAppSync.h"
#include "SmatchetHelpMarker.h"
#include "Sync/CacheRecreateReportPure.h"
#include "Ui/SmatchetStorageSnapshotPure.h"
#include "Ui/TouchInputRouter.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "Ui/SmatchetDialogScope.h"
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
void DrawGeneralUpdatesSection(const IAppMeta& meta, UiDrawSession& d) {
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
            d.appUpdateFuture = std::async(std::launch::async, [&meta, cfg = d.cfg]() {
                return meta.CheckForAppUpdate(cfg.UpdateIncludePrerelease);
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
        smatchet::ui::touch::MarkLastItemAsTouchDragSurface(); // finger drag moves the slider, not the page
        ImGui::SetItemTooltip("Days before compact dates switch to absolute.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.appearance.date_threshold.help",
                                   "Threshold in days where the compact view transitions from relative "
                                   "(e.g. -3d) to short absolute (e.g. May 07 '26).");
    }
}

// Storage: Portable/Shared storage-mode combo and the restart prompt.
void DrawGeneralStorageSection(const IAppMeta& meta, UiDrawSession& d) {
    // `meta` (RequestAppQuit) is only reached on the non-Unreal restart path below; in the
    // SMATCHET_EMBEDDED_IN_UNREAL build that block is compiled out, leaving it unreferenced.
    (void)meta;
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
    // Pillar 2 (#2044): the marker-file stat + parse and the two displayed paths are resolved
    // ONCE per (Preferences-window open × runtime-asset dir), not per frame. The predicate is
    // the cache key — a frame-count throttle would still be wrong, just less often.
    if (smatchet::prefs_storage::StorageSnapshotStale(d.storageSnapshotValid, d.storageSnapshotRuntimeAssetDir,
                                                      runtimeAssetDir)) {
        const ConfigManager::StoragePreference resolvedPref =
            ConfigManager::GetStoragePreference(runtimeAssetDir, kDefaultPref);
        d.storageSnapshotPrefIndex = (resolvedPref == ConfigManager::StoragePreference::Portable) ? 0 : 1;
        d.storageSnapshotUserDataDir = ConfigManager::GetUserDataDirectory();
        d.storageSnapshotMarkerPath = ConfigManager::GetStoragePreferenceFlagPath(runtimeAssetDir);
        d.storageSnapshotRuntimeAssetDir = runtimeAssetDir;
        d.storageSnapshotValid = true;
    }
    int prefIndex = d.storageSnapshotPrefIndex;
    const char* items[] = {SmatchetLocalization::T("prefs.storage.portable", "Portable (next to runtime files)"),
                           SmatchetLocalization::T("prefs.storage.shared", "Shared (OS user-data folder)")};
    static bool s_storageModeChanged = false;
    if (ImGui::Combo("Storage mode", &prefIndex, items, IM_ARRAYSIZE(items))) {
        const ConfigManager::StoragePreference chosen =
            (prefIndex == 0) ? ConfigManager::StoragePreference::Portable : ConfigManager::StoragePreference::Shared;
        const VoidResult stored = ConfigManager::SetStoragePreference(runtimeAssetDir, chosen);
        if (stored.has_value()) {
            s_storageModeChanged = true;
            // The marker file just changed underneath the snapshot — re-resolve next frame
            // (rather than assuming `chosen` landed verbatim; the file is the authority).
            d.storageSnapshotValid = false;
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
                meta.RequestAppQuit();
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
    ImGui::TextDisabled("Current writable directory: %s", d.storageSnapshotUserDataDir.c_str());
    ImGui::TextDisabled("Marker file: %s", d.storageSnapshotMarkerPath.c_str());
}

// Local database: intro blurb, resolved cache path, the "Recreate database..."
// button and its confirm modal (modal stays whole with its opener).
// Where the cache came from: copied from an older location, or an older file borrowed for this session; plus the
// other older caches that were left alone. Nothing here changes a file.
void DrawLocalCacheLocationLines(const smatchet::persistence::LocalCacheStatus& status) {
    if (status.UsingLegacyPath) {
        ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                     "prefs.local_db.using_legacy",
                                     "This session is using the cache file left at %s because it could not be copied "
                                     "to the current location just now. Restart Smatchet to move it.",
                                     status.LegacyPath.c_str()));
    } else if (!status.ImportedFrom.empty()) {
        ImGui::TextWrapped(
            "%s", SmatchetLocalization::Format("prefs.local_db.imported_from",
                                               "Copied from the cache file left at %s. That file was not changed.",
                                               status.ImportedFrom.c_str()));
    }
    if (status.LastRecovery.Rebuilt) {
        // Kept after the one-time notice is gone: the damaged copy still holds whatever could not be carried over.
        ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                     "prefs.local_db.last_rebuilt",
                                     "The database was damaged and has been rebuilt. The damaged copy is kept at %s.",
                                     status.LastRecovery.QuarantinedPath.c_str()));
    }
    for (std::size_t i = 0; i < status.OtherLegacyCaches.size(); ++i) {
        ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                     "prefs.local_db.other_caches",
                                     "Another older cache file was found at %s and was not used. It was not changed.",
                                     status.OtherLegacyCaches[i].c_str()));
    }
}

void DrawGeneralLocalDatabaseSection(IAppSync& sync, UiDrawSession& d) {
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
    const smatchet::persistence::LocalCacheStatus cacheStatus = sync.GetLocalCacheStatus();
    // While an older file is borrowed, that file (not the planned location) is the one in use.
    const std::string resolved = cacheStatus.UsingLegacyPath ? cacheStatus.Path : sync.GetResolvedLocalCacheDbPath();
    if (!resolved.empty()) {
        ImGui::TextDisabled("Cache file:");
        ImGui::SameLine();
        ::ImGui::TextWrapped("%s", resolved.c_str());
    }
    DrawLocalCacheLocationLines(cacheStatus);
    // A failed open leaves a memory-only cache for the session: say so here, because the toast is gone after a few
    // seconds, and keep Recreate off (the app refuses it too; there is no file to recreate).
    const bool sessionOnly = cacheStatus.State == smatchet::persistence::LocalCacheState::SessionOnly;
    if (sessionOnly) {
        ImGui::TextWrapped("%s",
                           SmatchetLocalization::Format(
                               "prefs.local_db.session_only",
                               "The database could not be opened (%s). Smatchet is using a temporary cache for this "
                               "session, so edits cannot be saved offline. Fix the cause and restart Smatchet.",
                               cacheStatus.LastError.c_str()));
    }
    if (sessionOnly) {
        // The retry runs on a worker behind the same gate as the first open, so the button is off while it runs.
        ImGui::BeginDisabled(sync.IsLocalCacheRecreating());
        if (ImGui::Button(SmatchetLocalization::T("prefs.local_db.retry", "Retry opening"))) {
            const VoidResult reopened = sync.StartReopenLocalCache();
            if (!reopened.has_value()) {
                SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.local_cache", "Local database"),
                                                      reopened.error(), ToastType::Warning, 6000);
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", SmatchetLocalization::T("prefs.local_db.retry_tooltip",
                                                            "Try the database file again without restarting."));
    }
    ImGui::Spacing();
    // The recreate runs on a worker behind a gate (IAppSync::StartRecreateLocalCacheDatabase); a second one cannot
    // start while it runs, and the cue says why the button is off.
    const bool recreating = sync.IsLocalCacheRecreating();
    const bool borrowed = cacheStatus.UsingLegacyPath;
    ImGui::BeginDisabled(recreating || sessionOnly || borrowed);
    if (ImGui::Button("Recreate database...")) {
        ImGui::OpenPopup("Delete local database?###RecreateSqliteDbConfirm");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "%s", sessionOnly ? SmatchetLocalization::T("prefs.local_db.recreate_unavailable",
                                                    "Unavailable while the database cannot be "
                                                    "opened: there is no file to recreate.")
              : borrowed  ? SmatchetLocalization::T("prefs.local_db.recreate_legacy",
                                                    "Unavailable while this session uses a cache file from an older "
                                                     "location. Restart Smatchet to move it first.")
                          : "Permanently delete the local cache file and start with an empty database.");
    if (recreating) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.local_db.recreating", "Recreating the database..."));
    }

    // Keys: smatchet::ui::ConfirmFooter. Destructive, so Cancel takes the keyboard on open and Enter cancels.
    static smatchet::ui::DialogScopeState s_recreateDialog;
    if (smatchet::ui::BeginDialog(s_recreateDialog, "Delete local database?###RecreateSqliteDbConfirm", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "This removes cached issues and any queued offline writes stored on this machine. It does not "
            "delete anything on the tracker. Continue?");
        ImGui::Separator();
        // P2-M16: action-first order + the shared destructive styling, now drawn by the shared footer.
        const bool deleteClicked =
            smatchet::ui::ConfirmFooter(s_recreateDialog, smatchet::ui::DestructiveFooter("Delete and recreate")) ==
            smatchet::dialogkeys::ConfirmPick::Primary;
        if (deleteClicked) {
            // The wipe is asynchronous: the completion (AppController) pushes the success or failure toast and, on
            // success, starts the follow-up full refresh once the new database is in place.
            const VoidResult started = sync.StartRecreateLocalCacheDatabase(nullptr);
            if (started.has_value()) {
                ImGui::CloseCurrentPopup();
            } else {
                // Refused before anything changed (no database path, shutting down, one already running).
                const char* detail = SmatchetLocalization::T("toast.local_db_recreate_failed_detail",
                                                             "Could not recreate the local database.");
                SmatchetToastManager::Instance().Push(
                    std::string(SmatchetLocalization::T("toast.local_db_error_title", "Local database")),
                    started.error().empty() ? std::string(detail)
                                            : smatchet::sync::DescribeCacheRecreateFailure(false, started.error()),
                    ToastType::Error, 6000);
            }
        }
        smatchet::ui::EndDialog(s_recreateDialog);
    }
}

} // namespace

void DrawGeneralPreferencesTab(const IAppMeta& meta, IAppSync& sync, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.updates", [&] { DrawGeneralUpdatesSection(meta, d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.language_region",
                                              [&] { DrawGeneralLanguageRegionSection(d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.storage", [&] { DrawGeneralStorageSection(meta, d); });
    SmatchetPreferencesUiDetail::PrefsSection(d, "general.local_database",
                                              [&] { DrawGeneralLocalDatabaseSection(sync, d); });
}
