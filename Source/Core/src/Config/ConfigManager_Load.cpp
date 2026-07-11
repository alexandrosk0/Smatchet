// ConfigManager::Load(CliOverrides) + LoadAnnotateAnalysis and the load-side field / enum / list /
// migration helpers, split out of ConfigManager.cpp for the god-file-splits partition. Behavior-
// identical body move; scalar-field and secret entry points come from config_detail.

#include "ConfigManager.h"
#include "ConfigManager_Internal.h"

#include "Logger.h"
#include "NewIssueInheritDefaults.h"
#include "SmatchetDefaults.h"

// Full nlohmann::json is needed here because we (a) define CommentTemplate's friend serializers
// and (b) construct json values in the view-disk helpers and the per-method bodies below. The
// public header (ConfigManager.h) only pulls <nlohmann/json_fwd.hpp> — every consumer pays the
// 75-LOC fwd-decl parse cost instead of the full 30k-LOC json.hpp.
#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared ConfigManager-TU include block + config_detail using-prologue is grandfathered across the god-file-split siblings (ConfigManager.cpp / _Save / _Load / _Secrets) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared ConfigManager TU prologue header is introduced)
// clang-format on

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>

using smatchet::config_detail::GetCachedConfigRef;
using smatchet::config_detail::GetCacheMutexRef;
using smatchet::config_detail::GetConfigRmwMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;
using smatchet::config_detail::SanitizeConfigStringValue;

#if defined(_WIN32) || defined(__ANDROID__)
using smatchet::config_detail::ProtectSecretForConfig;
using smatchet::config_detail::UnprotectSecretFieldFromConfig;
#endif

using smatchet::config_detail::LoadScalarFields;
using smatchet::config_detail::LoadSecretFields;
using smatchet::config_detail::SecretMigrationFlags;

namespace {

#if defined(SMATCHET_WITH_WHISPER)
// Whisper non-secret fields + the WhisperMaxClipSec clamp. (WhisperApiKey loads in LoadSecretFields.)
void LoadWhisperFields(const nlohmann::json& j, TrackerConfig& cfg) {
    cfg.WhisperEnabled = j.value("whisper_enabled", cfg.WhisperEnabled);
    cfg.WhisperSetupCompleted = j.value("whisper_setup_completed", cfg.WhisperSetupCompleted);
    cfg.WhisperSetupChoice = j.value("whisper_setup_choice", cfg.WhisperSetupChoice);
    cfg.WhisperMode = j.value("whisper_mode", cfg.WhisperMode);
    cfg.WhisperModel = j.value("whisper_model", cfg.WhisperModel);
    cfg.WhisperHotkey = j.value("whisper_hotkey", cfg.WhisperHotkey);
    cfg.WhisperConsentTimestampSec = j.value("whisper_consent_timestamp_sec", cfg.WhisperConsentTimestampSec);
    cfg.WhisperLanguage = j.value("whisper_language", cfg.WhisperLanguage);
    cfg.WhisperTrim = j.value("whisper_trim", cfg.WhisperTrim);
    cfg.WhisperMaxClipSec = j.value("whisper_max_clip_sec", cfg.WhisperMaxClipSec);
    cfg.WhisperAutoSendOnPunctuation = j.value("whisper_auto_send_on_punctuation", cfg.WhisperAutoSendOnPunctuation);
    // Clamp WhisperMaxClipSec into the documented range so a hand-edited config can't drive
    // runaway cloud cost. 0 disables; positive values clamp at 600 s.
    if (cfg.WhisperMaxClipSec < 0) {
        cfg.WhisperMaxClipSec = 0;
    } else if (cfg.WhisperMaxClipSec > 600) {
        cfg.WhisperMaxClipSec = 600;
    }
}
#endif

// Enum / clamped scalar fields that don't fit the plain-scalar table: ai_provider_kind clamp,
// assistant_history_max_rows clamp, date-threshold clamp, layout-schema clamp, font-size clamp,
// ui_density / panel_position / theme string<->enum, ui_language normalization.
void LoadEnumAndClampedFields(const nlohmann::json& j, TrackerConfig& cfg) {
    // Clamp `ai_provider_kind` to the known enum range; a future-version persisted int (e.g. 99)
    // on an older build degrades to OpenAi (0) rather than triggering UB on enum cast.
    {
        const int rawProvider = j.value("ai_provider_kind", cfg.AiProviderKind);
        const int kProviderMin = static_cast<int>(AiProvider::OpenAi);
        const int kProviderMax = static_cast<int>(AiProvider::DeepSeek);
        cfg.AiProviderKind = (rawProvider < kProviderMin || rawProvider > kProviderMax)
                                 ? static_cast<int>(AiProvider::OpenAi)
                                 : rawProvider;
    }
    // Clamp on load — negative / zero would silently disable history; a stray very-large value
    // would let the SQLite file grow without bound. Cap at 100k.
    const int rawMaxRows = j.value("assistant_history_max_rows", cfg.AssistantHistoryMaxRows);
    cfg.AssistantHistoryMaxRows = (rawMaxRows < 1) ? 1 : (rawMaxRows > 100000 ? 100000 : rawMaxRows);

    if (cfg.DateCompactRelativeThresholdDays < 1)
        cfg.DateCompactRelativeThresholdDays = 1;
    if (cfg.DateCompactRelativeThresholdDays > 365)
        cfg.DateCompactRelativeThresholdDays = 365;

    // User Info window — zero/negative would silently empty the activity feed / VCS lists.
    if (cfg.UserActivityDayWindow < 1) {
        cfg.UserActivityDayWindow = 1;
    }
    if (cfg.MaxUserChanges < 1) {
        cfg.MaxUserChanges = 1;
    }
    // Ticket-change monitor — clamp the poll interval to a sane band. Below 30 s the
    // backend gets hammered; above 1 h the monitor is effectively off.
    if (cfg.TicketChangeMonitorIntervalSec < 30) {
        cfg.TicketChangeMonitorIntervalSec = 30;
    }
    if (cfg.TicketChangeMonitorIntervalSec > 3600) {
        cfg.TicketChangeMonitorIntervalSec = 3600;
    }
    // Unknown / hand-edited layout values degrade to the fresh-install default.
    if (cfg.VcsFeedLayout != "unified" && cfg.VcsFeedLayout != "separate") {
        cfg.VcsFeedLayout = "unified";
    }

    cfg.LayoutSchemaVersion = j.value("layout_schema_version", 0);
    if (cfg.LayoutSchemaVersion < 0) {
        cfg.LayoutSchemaVersion = 0;
    }
    cfg.FontSizePt = j.value("font_size_pt", cfg.FontSizePt);
    if (cfg.FontSizePt < SmatchetDefaults::kFontSizeMinPt) {
        cfg.FontSizePt = SmatchetDefaults::kFontSizeMinPt;
    }
    if (cfg.FontSizePt > SmatchetDefaults::kFontSizeMaxPt) {
        cfg.FontSizePt = SmatchetDefaults::kFontSizeMaxPt;
    }
    {
        const std::string densityStr = j.value("ui_density", std::string("Normal"));
        if (densityStr == "Compact")
            cfg.Density = TrackerConfig::UiDensity::Compact;
        else if (densityStr == "Comfortable")
            cfg.Density = TrackerConfig::UiDensity::Comfortable;
        else
            cfg.Density = TrackerConfig::UiDensity::Normal;
    }
    {
        const std::string panelPosStr = j.value("panel_position", std::string("Bottom"));
        cfg.PanelDockSide =
            (panelPosStr == "Right") ? TrackerConfig::PanelPosition::Right : TrackerConfig::PanelPosition::Bottom;
    }
    // Default-string mirrors the construct-time default in TrackerConfig::Theme — fresh installs
    // (no `theme` key) land on ImGuiDefaultDark; existing configs round-trip their persisted value.
    const std::string themeStr = j.value("theme", std::string("ImGuiDefaultDark"));
    if (themeStr == "ModernDark")
        cfg.Theme = ThemeId::ModernDark;
    else if (themeStr == "Vs2022Dark")
        cfg.Theme = ThemeId::Vs2022Dark;
    else if (themeStr == "Vs2022Light")
        cfg.Theme = ThemeId::Vs2022Light;
    else if (themeStr == "HighContrast")
        cfg.Theme = ThemeId::HighContrast;
    else if (themeStr == "NortonCommander")
        cfg.Theme = ThemeId::NortonCommander;
    else if (themeStr == "SmatchetDark")
        cfg.Theme = ThemeId::SmatchetDark;
    else
        // Covers the literal "ImGuiDefaultDark" string AND any unknown / future value —
        // unknown serialized themes degrade to the fresh-install default.
        cfg.Theme = ThemeId::ImGuiDefaultDark;
    // UI layout mode + mobile-shell prefs. All optional with defaults; unknown
    // serialized values degrade to the fresh-install default via *FromString.
    cfg.UiMode = uiModeFromString(j.value("uiMode", std::string("auto")));
    if (j.contains("mobileNav") && j["mobileNav"].is_array()) {
        cfg.MobileNavPages.clear();
        for (const auto& item : j["mobileNav"]) {
            if (item.is_string()) {
                cfg.MobileNavPages.push_back(item.get<std::string>());
            }
        }
    }
    cfg.MobileHomePage = j.value("mobileHome", cfg.MobileHomePage);
    cfg.MobileTouchDensity = mobileTouchDensityFromString(j.value("mobileDensity", std::string("comfortable")));
    ConfigManager::SanitizeMobileNav(cfg);
    cfg.UiLanguage = ConfigManager::NormalizeUiLanguageCode(j.value("ui_language", cfg.UiLanguage));
}

// Trim leading/trailing ASCII space+tab in place (used by the inherit-field-id list parser).
void TrimAsciiSpace(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
}

// Parse one "new_issue_inherit_field_ids*" array: trim each entry, drop empties + "summary",
// fall back to the default list when the key is absent or yields an empty result.
void LoadInheritFieldIds(const nlohmann::json& j, const char* key, std::vector<std::string>& out) {
    out = DefaultNewIssueInheritFieldIdsList();
    if (j.contains(key) && j[key].is_array()) {
        out.clear();
        for (const auto& item : j[key]) {
            if (item.is_string()) {
                std::string s = item.get<std::string>();
                TrimAsciiSpace(s);
                if (!s.empty() && s != "summary") {
                    out.push_back(std::move(s));
                }
            }
        }
    }
    if (out.empty()) {
        out = DefaultNewIssueInheritFieldIdsList();
    }
}

// One-shot migration: fold the legacy BugReportHotkey / BugReportHotkeyEnabled pair into the
// keybinding registry ("app.bug_report.open"). The registry became authoritative for the
// bug-report shortcut when it became rebindable, but Defaults() seeded "Ctrl+Shift+B" regardless of
// a user's customized / disabled BugReportHotkey — fold it once so that customization survives.
// Runs after the keybindings load (cfg.Keybindings populated) and after LoadScalarFields
// (cfg.BugReportHotkey* populated). SetBindingHotkey upserts the "{}" binding in place.
void MigrateBugReportHotkeyToKeybindings(const nlohmann::json& j, TrackerConfig& cfg) {
    cfg.MigratedBugReportHotkeyV1 = j.value("migrated_bugreport_hotkey_v1", false);
    if (cfg.MigratedBugReportHotkeyV1) {
        return;
    }
    // Only fold the legacy pair forward when it actually diverged from the seeded default. A fresh
    // profile (no config file) skips this migration entirely — LoadListFields runs only when the
    // loaded JSON is non-empty — so the flag stays false; a user who then rebinds
    // "app.bug_report.open" via the editor and saves would, on the NEXT load, have that
    // customization clobbered here by the default legacy hotkey. Guarding on legacy-customized
    // keeps the rebind intact while still carrying a genuinely customized / disabled legacy hotkey
    // forward exactly once. ("Ctrl+Shift+B" mirrors the BugReportHotkey default in ConfigManager.h
    // and the app.bug_report.open seed in KeybindingsConfig::Defaults().)
    const bool legacyCustomized = cfg.BugReportHotkey != "Ctrl+Shift+B" || !cfg.BugReportHotkeyEnabled;
    if (legacyCustomized) {
        cfg.Keybindings.SetBindingHotkey("app.bug_report.open", "{}", cfg.BugReportHotkey);
        if (!cfg.BugReportHotkeyEnabled) {
            const int idx = cfg.Keybindings.FindBindingIndex("app.bug_report.open", "{}");
            if (idx >= 0) {
                cfg.Keybindings.Bindings[static_cast<std::size_t>(idx)].Enabled = false;
            }
        }
    }
    cfg.MigratedBugReportHotkeyV1 = true;
}

// One-shot migration: seed the menu-bar shortcut keybindings introduced with the
// menu-shortcuts-fix work (Zoom In/Out/Reset, open-view, clear-selection, and the new view
// reveals). from_json REPLACES (does not merge) the binding table, so a config saved by an older
// build keeps only the bindings it knew about — an upgrading user would never receive these new
// defaults (the marquee "Zoom In doesn't work" symptom). Seed each new default exactly once
// (guarded by migrated_menu_shortcuts_v1), and only when the loaded config has no binding for that
// exact (command id, args) identity — so a user who already bound the command to their own key in
// this build keeps it, and a re-run never resurrects a binding the user later cleared. A fresh
// profile takes KeybindingsConfig::Defaults() wholesale (the else-branch below) and records the
// flag on first save, so the seed is a no-op for new users.
void MigrateMenuShortcutKeybindingsV1(const nlohmann::json& j, TrackerConfig& cfg) {
    cfg.MigratedMenuShortcutsV1 = j.value("migrated_menu_shortcuts_v1", false);
    if (cfg.MigratedMenuShortcutsV1) {
        return;
    }
    static const char* const kNewCommandIds[] = {"ui.zoom.in",
                                                 "ui.zoom.out",
                                                 "ui.zoom.reset",
                                                 "ui.open_view",
                                                 "grid.clear_selection",
                                                 "view.toggle.views_dashboard",
                                                 "view.toggle.log",
                                                 "view.toggle.backend_audit",
                                                 "view.toggle.source_annotate"};
    const KeybindingsConfig defaults = KeybindingsConfig::Defaults();
    int seeded = 0;
    for (const Keybinding& def : defaults.Bindings) {
        bool isNew = false;
        for (const char* id : kNewCommandIds) {
            if (def.CommandId == id) {
                isNew = true;
                break;
            }
        }
        if (isNew && cfg.Keybindings.FindBindingIndex(def.CommandId, def.ArgsJson) < 0) {
            cfg.Keybindings.Bindings.push_back(def);
            ++seeded;
        }
    }
    if (seeded > 0) {
        LOG_INFO("ConfigManager: seeded %d new menu-shortcut keybinding(s) into an existing config "
                 "(migrated_menu_shortcuts_v1)",
                 seeded);
    }
    cfg.MigratedMenuShortcutsV1 = true;
}

// List + nested-object fields: mcp_export_fields, comment-template arrays, duration/worklog
// suggestion lists, the three inherit-id lists, and the one-shot issuetype-inject migration.
void LoadListFields(const nlohmann::json& j, TrackerConfig& cfg) {
    if (j.contains("mcp_export_fields") && j["mcp_export_fields"].is_array()) {
        for (const auto& item : j["mcp_export_fields"]) {
            if (item.is_string()) {
                cfg.McpExportFields.push_back(item.get<std::string>());
            }
        }
    }

    if (j.contains("quick_comment_templates") && j["quick_comment_templates"].is_array()) {
        cfg.QuickCommentTemplates.clear();
        for (const auto& item : j["quick_comment_templates"]) {
            try {
                cfg.QuickCommentTemplates.push_back(item.get<CommentTemplate>());
            } catch (...) { // catch-all-ok: skip malformed template entry
            }
        }
    } else {
        cfg.QuickCommentTemplates = GetDefaultQuickCommentTemplates();
    }

    if (j.contains("toolbar") && j["toolbar"].is_object()) {
        try {
            cfg.Toolbar = j["toolbar"].get<ToolbarConfig>();
        } catch (...) { // catch-all-ok: malformed toolbar block → defaults
            cfg.Toolbar = ToolbarConfig::Default();
        }
    } else {
        cfg.Toolbar = ToolbarConfig::Default();
    }

    if (j.contains("keybindings") && j["keybindings"].is_object()) {
        try {
            cfg.Keybindings = j["keybindings"].get<KeybindingsConfig>();
        } catch (...) { // catch-all-ok: malformed keybindings block → defaults
            cfg.Keybindings = KeybindingsConfig::Defaults();
        }
    } else {
        cfg.Keybindings = KeybindingsConfig::Defaults();
    }

    if (j.contains("annotate_comment_templates") && j["annotate_comment_templates"].is_array()) {
        cfg.AnnotateCommentTemplates.clear();
        for (const auto& item : j["annotate_comment_templates"]) {
            try {
                cfg.AnnotateCommentTemplates.push_back(item.get<CommentTemplate>());
            } catch (...) { // catch-all-ok: skip malformed template entry
            }
        }
    } else {
        cfg.AnnotateCommentTemplates = GetDefaultAnnotateCommentTemplates();
    }

    if (j.contains("duration_suggestions") && j["duration_suggestions"].is_array()) {
        cfg.DurationSuggestions.clear();
        for (const auto& item : j["duration_suggestions"]) {
            if (item.is_string()) {
                cfg.DurationSuggestions.push_back(item.get<std::string>());
            }
        }
    } else {
        cfg.DurationSuggestions = {"15m", "30m", "1h", "2h", "4h", "8h", "1d", "2d", "1w"};
    }

    if (j.contains("worklog_comment_templates") && j["worklog_comment_templates"].is_array()) {
        cfg.WorkLogCommentTemplates.clear();
        for (const auto& item : j["worklog_comment_templates"]) {
            if (item.is_string()) {
                cfg.WorkLogCommentTemplates.push_back(item.get<std::string>());
            }
        }
    } else {
        cfg.WorkLogCommentTemplates = {
            "Investigated and resolved the issue.", "Tested and verified on local environment.",
            "Refactored code and ran static analysis.", "Discussed with team and updated implementation.",
            "Wrote unit tests and verified all passing."};
    }

    LoadInheritFieldIds(j, "new_issue_inherit_field_ids", cfg.NewIssueInheritFieldIds);
    LoadInheritFieldIds(j, "new_issue_inherit_field_ids_plane", cfg.NewIssueInheritFieldIdsPlane);
    LoadInheritFieldIds(j, "new_issue_inherit_field_ids_github", cfg.NewIssueInheritFieldIdsGitHub);
    LoadInheritFieldIds(j, "new_issue_inherit_field_ids_linear", cfg.NewIssueInheritFieldIdsLinear);

    // One-shot migration: inject "issuetype" into the front of all inherit lists if absent.
    cfg.MigratedInheritIssueTypeV1 = j.value("migrated_inherit_issuetype_v1", false);
    if (!cfg.MigratedInheritIssueTypeV1) {
        const auto injectIfMissing = [](std::vector<std::string>& list) {
            if (std::find(list.begin(), list.end(), std::string("issuetype")) == list.end()) {
                list.insert(list.begin(), "issuetype");
            }
        };
        injectIfMissing(cfg.NewIssueInheritFieldIds);
        injectIfMissing(cfg.NewIssueInheritFieldIdsPlane);
        injectIfMissing(cfg.NewIssueInheritFieldIdsGitHub);
        injectIfMissing(cfg.NewIssueInheritFieldIdsLinear);
        cfg.MigratedInheritIssueTypeV1 = true;
    }

    MigrateBugReportHotkeyToKeybindings(j, cfg);
    MigrateMenuShortcutKeybindingsV1(j, cfg);
}

// Route SMATCHET_TRACKER_TOKEN / SMATCHET_TRACKER_BASE_URL to the active backend's
// credential + origin-URL slots. All three non-Jira arms route off trackerTypeLower:
// the in-app-persisted canonical value is PascalCase ("GitHub"/"Plane"/"Linear"), but
// smatchet_config.json can be hand-edited with lowercase values — DefaultTrackerBackendFactory
// already does a case-insensitive match when selecting the live backend (see
// SmatchetPreferencesUi.cpp's "load path doesn't canonicalize" comment), so comparing raw
// casing here silently fell through to the Jira/default slot for a hand-edited config, exactly
// the CPP_CODE_AUDIT.md #2 class (originally reported for GitHub only; Plane has the same gap).
// Extracted from ApplyOverridesAndClamps to keep that function under the branch cap.
static void RouteTrackerEnvCredentials(TrackerConfig& cfg) {
    std::string trackerTypeLower = cfg.TrackerType;
    std::transform(trackerTypeLower.begin(), trackerTypeLower.end(), trackerTypeLower.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });

    // SMATCHET_TRACKER_TOKEN — tracker API credential (Jira ApiToken / Plane ApiKey / GitHub PAT / Linear key).
    if (const char* envToken = std::getenv("SMATCHET_TRACKER_TOKEN")) {
        if (envToken[0] != '\0') {
            if (trackerTypeLower == "plane") {
                cfg.PlaneApiKey = envToken;
            } else if (trackerTypeLower == "github") {
                cfg.GitHubPat = envToken;
            } else if (trackerTypeLower == "linear") {
                cfg.LinearApiKey = envToken;
            } else {
                cfg.ApiToken = envToken;
            }
        }
    }

    // SMATCHET_TRACKER_BASE_URL — tracker origin URL (Jira→Domain / Plane→PlaneUrl / GitHub→GitHubBaseUrl /
    // Linear→LinearBaseUrl).
    if (const char* envBase = std::getenv("SMATCHET_TRACKER_BASE_URL")) {
        if (envBase[0] != '\0') {
            if (trackerTypeLower == "plane") {
                cfg.PlaneUrl = envBase;
            } else if (trackerTypeLower == "github") {
                cfg.GitHubBaseUrl = envBase;
            } else if (trackerTypeLower == "linear") {
                cfg.LinearBaseUrl = envBase;
            } else {
                cfg.Domain = envBase;
            }
        }
    }
}

// Env-var + CLI overrides applied post-disk-read, plus the final post-override safety clamps.
void ApplyOverridesAndClamps(const ConfigManager::CliOverrides& cli, TrackerConfig& cfg) {
    if (const char* envDbPath = std::getenv("SMATCHET_DB_PATH")) {
        cfg.DbPath = envDbPath;
    }
    if (const char* envBackend = std::getenv("SMATCHET_BACKEND_TYPE")) {
        cfg.TrackerType = envBackend;
    } else if (const char* envTracker = std::getenv("SMATCHET_TRACKER_TYPE")) {
        cfg.TrackerType = envTracker;
    }
    if (const char* envMcpPort = std::getenv("SMATCHET_MCP_PORT")) {
        try {
            cfg.McpPort = std::stoi(envMcpPort);
        } catch (...) { // catch-all-ok: stoi on env var — keep default port
        }
    }
    if (const char* envMcpRemote = std::getenv("SMATCHET_MCP_ALLOW_REMOTE")) {
        std::string s(envMcpRemote);
        cfg.McpAllowRemote = (s == "true" || s == "1");
    }
    // SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK — opt out of the tokenless-loopback
    // 401 while keeping the secure default ON. Used by single-tenant CI runners
    // that --spawn an ephemeral child and drive it over loopback MCP without
    // provisioning a token; on a throwaway runner "any local process" is just the
    // CI job itself, so the defence the default adds (a co-resident process
    // reaching the registry) does not apply. This flag gates UNAUTHENTICATED
    // local MCP access, so it fails CLOSED: only an explicit "false"/"0" disables
    // it; an unset, empty, or malformed value preserves the secure default.
    if (const char* envReqTok = std::getenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK")) {
        std::string s(envReqTok);
        if (s == "true" || s == "1") {
            cfg.McpRequireTokenOnLoopback = true;
        } else if (s == "false" || s == "0") {
            cfg.McpRequireTokenOnLoopback = false;
        } else if (!s.empty()) {
            LOG_WARN(
                "ConfigManager: ignoring invalid SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK=%s (keeping secure default)",
                s.c_str());
        }
    }

    // Route SMATCHET_TRACKER_TOKEN / SMATCHET_TRACKER_BASE_URL to the active backend's cfg slots.
    RouteTrackerEnvCredentials(cfg);

    // SMATCHET_LOG_LEVEL — minimum log verbosity: trace|debug|info|warn|error.
    if (const char* envLog = std::getenv("SMATCHET_LOG_LEVEL")) {
        if (envLog[0] != '\0') {
            cfg.LogMinLevel = envLog;
        }
    }

    // Option 4 overrides (CLI parameters) — win over env.
    if (cli.HasDbPath) {
        cfg.DbPath = cli.DbPath;
    }
    if (cli.HasBackendType) {
        cfg.TrackerType = cli.BackendType;
    }
    if (cli.HasMcpPort) {
        cfg.McpPort = cli.McpPort;
    }
    if (cli.HasMcpAllowRemote) {
        cfg.McpAllowRemote = cli.McpAllowRemote;
    }

    // Post-override clamp and safety bounds checking.
    if (cfg.ImportMaxConcurrent < 1)
        cfg.ImportMaxConcurrent = 1;
    if (cfg.ImportMaxConcurrent > 32)
        cfg.ImportMaxConcurrent = 32;
    if (cfg.GridEndWheelSwallowsBeforeHorizontal < 0)
        cfg.GridEndWheelSwallowsBeforeHorizontal = 0;
    if (cfg.GridEndWheelSwallowsBeforeHorizontal > 32)
        cfg.GridEndWheelSwallowsBeforeHorizontal = 32;
    if (cfg.McpPort < 1 || cfg.McpPort > 65535) {
        cfg.McpPort = SmatchetDefaults::Mcp::kDefaultPort;
    }
}

} // namespace

// ConfigManager — annotate-analysis persistence.

AnnotateAnalysisConfig ConfigManager::LoadAnnotateAnalysis() {
    nlohmann::json j = LoadMergedConfigJson();
    AnnotateAnalysisConfig b;
    if (!j.contains("annotate_analysis") || !j["annotate_analysis"].is_object()) {
        return b;
    }
    const nlohmann::json& ba = j["annotate_analysis"];
    b.P4Executable = ba.value("p4_exe", b.P4Executable);
    b.P4VcExecutable = ba.value("p4vc_exe", b.P4VcExecutable);
    b.TimelapseCommandTemplate = ba.value("timelapse_cmd", std::string());
    b.ChangeCommandTemplate = ba.value("change_cmd", std::string());
    b.AiChatUrl = ba.value("ai_chat_url", std::string());
    b.DefaultMaxFrames = ba.value("default_max_frames", b.DefaultMaxFrames);
    b.ChangelistCacheMaxEntries = ba.value("cl_cache_max", b.ChangelistCacheMaxEntries);
    // Clamp on load so a hand-edited out-of-range value can't reach the worker cache sizing
    // (AnnotateAnalysisUi_Worker.cpp). Mirrors the UI InputInt range (16..8192).
    if (b.ChangelistCacheMaxEntries < 16) {
        b.ChangelistCacheMaxEntries = 16;
    } else if (b.ChangelistCacheMaxEntries > 8192) {
        b.ChangelistCacheMaxEntries = 8192;
    }
    b.ShowRawCallstack = ba.value("show_raw_callstack", b.ShowRawCallstack);
    b.CallstackTrackerFieldId = ba.value("callstack_jira_field_id", std::string());
    b.LastFoundClTrackerFieldId = ba.value("last_found_cl_jira_field_id", std::string());
    b.LastOccurrencesTrackerFieldId = ba.value("last_occurrences_jira_field_id", std::string());
    if (ba.contains("default_ignore_keywords") && ba["default_ignore_keywords"].is_array()) {
        for (const auto& item : ba["default_ignore_keywords"]) {
            if (item.is_string()) {
                b.DefaultIgnoreKeywords.push_back(item.get<std::string>());
            }
        }
    }
    if (ba.contains("path_remaps") && ba["path_remaps"].is_array()) {
        for (const auto& rule : ba["path_remaps"]) {
            if (!rule.is_object()) {
                continue;
            }
            PathRemapRule r;
            r.FromPrefix = rule.value("from", std::string());
            r.ToPrefix = rule.value("to", std::string());
            if (!r.FromPrefix.empty()) {
                b.PathRemaps.push_back(std::move(r));
            }
        }
    }
    if (ba.contains("ui_colors") && ba["ui_colors"].is_object()) {
        const nlohmann::json& uc = ba["ui_colors"];
        auto loadRgba = [&uc](const char* key, float* out) {
            if (!uc.contains(key) || !uc[key].is_array() || uc[key].size() < 4) {
                return;
            }
            try {
                float tmp[4];
                for (int i = 0; i < 4; ++i) {
                    tmp[i] = static_cast<float>(uc[key][i].get<double>());
                }
                std::memcpy(out, tmp, sizeof(tmp));
            } catch (...) { // catch-all-ok: malformed RGBA in config — keep default color
            }
        };
        loadRgba("status_info", b.UiColors.StatusInfo);
        loadRgba("status_error", b.UiColors.StatusError);
        loadRgba("status_warning", b.UiColors.StatusWarning);
        loadRgba("find_highlight", b.UiColors.FindHighlight);
        loadRgba("text_disabled", b.UiColors.TextDisabled);
        loadRgba("import_existing", b.UiColors.ImportExisting);
        loadRgba("cl_tooltip_title", b.UiColors.ClTooltipTitle);
    }
    return b;
}

// ConfigManager — Load(CliOverrides).

TrackerConfig ConfigManager::Load(const CliOverrides& cli) {
    const bool canUseCache = !cli.HasDbPath && !cli.HasBackendType && !cli.HasMcpPort && !cli.HasMcpAllowRemote;
    if (canUseCache) {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        // cppcheck-suppress knownConditionTrueFalse ; cache flag is set by Invalidate/Store paths cppcheck does not
        // model
        if (GetHasCachedConfigRef()) {
            return GetCachedConfigRef();
        }
    }

    nlohmann::json j = LoadMergedConfigJson();
    const bool hasSetupConfig = !LoadJsonFile(GetConfigPath()).empty();
    TrackerConfig cfg;
    cfg.DbPath = SmatchetDefaults::kDefaultDbPath;
    cfg.TrackerType = SmatchetDefaults::kDefaultBackendType;
    cfg.McpPort = SmatchetDefaults::Mcp::kDefaultPort;
    SecretMigrationFlags migrate;

    if (!j.empty()) {
        // Load each field group under its own try/catch so a single type-mismatched key (for
        // example a string where a number is expected) can drop only that one group, never abort
        // the secret or list groups that follow. A shared try/catch previously let one bad scalar
        // skip secret loading, leaving the secrets empty, which a later save then persisted as
        // empty and permanently wiped the stored tokens.
        const auto loadGroup = [](const char* group, auto&& fn) {
            try {
                fn();
            } catch (const std::exception& ex) {
                LOG_ERROR("ConfigManager: Load() parse error in %s: %s", group, ex.what());
            } catch (...) {
                LOG_ERROR("ConfigManager: Load() parse error in %s (unknown)", group);
            }
        };
        loadGroup("scalar fields", [&] { LoadScalarFields(j, cfg); });
        loadGroup("secret fields", [&] { LoadSecretFields(j, cfg, migrate); });
        loadGroup("enum and clamped fields", [&] { LoadEnumAndClampedFields(j, cfg); });
#if defined(SMATCHET_WITH_WHISPER)
        loadGroup("whisper fields", [&] { LoadWhisperFields(j, cfg); });
#endif
        loadGroup("list fields", [&] { LoadListFields(j, cfg); });
    }

    if (!hasSetupConfig && !j.contains("read_only_mode")) {
        cfg.ReadOnlyMode = true;
    }
    if (!hasSetupConfig && !j.contains("show_preferences_window")) {
        cfg.ShowPreferencesWindow = true;
    }

    // AgentsMdGlobalPath default-at-Load (vs default-at-construct): blank in-memory state means
    // "user has not picked a path"; only here do we know the platform shared dir. Resolving at
    // construct time would lose the ability to distinguish blank-by-default from blank-by-user.
    if (cfg.AgentsMdGlobalPath.empty()) {
        const std::string shared = GetPlatformSharedUserDataDirectory();
        if (!shared.empty()) {
            cfg.AgentsMdGlobalPath = shared + "agents.md";
        }
    }

#if defined(_WIN32) || defined(__ANDROID__)
    // Win32: only MCP gets an eager legacy cleanup here; older secrets keep their established lazy
    // migration behavior. Android (audit H2 / CR #1357): migrate.LegacyPlaintext is set for ANY
    // plaintext fallback, so this re-Save eagerly re-seals (or fail-closed drops) every legacy secret.
    // Ordering note, backlog #15: this migration re-save runs BEFORE the env/CLI override block below, by design.
    // - cfg here reflects what disk contained, with the legacy plaintext token already decoded into McpAuthToken.
    //   The re-save re-encrypts McpAuthToken into mcp_auth_token_enc on disk, which is the whole point of the
    //   migration.
    // - The env/CLI overrides DbPath, TrackerType, McpPort and McpAllowRemote are ephemeral and must NOT persist.
    //   They are meant to take effect this process only. Re-saving AFTER overrides would write override values
    //   back to disk and pollute the next launch when the env/CLI is no longer set.
    // - Consequence: for the rest of this Load, in-memory cfg (and the cache filled at the bottom) hold
    //   override-applied values while disk holds pre-override values. That divergence is intentional and matches
    //   pre-split behavior. The standing limitation that any subsequent re-save with this cfg would write
    //   override values to disk is a pre-existing concern outside the scope of this migration.
    if (migrate.Any()) {
#if defined(__ANDROID__)
        LOG_INFO("ConfigManager: migrating legacy plaintext secret(s) to Keystore-protected storage "
                 "(audit H2 fail-closed re-save: unseal-able secrets re-sealed, the rest dropped).");
#elif defined(SMATCHET_WITH_WHISPER)
        LOG_INFO("ConfigManager: migrating legacy plaintext secret(s) to DPAPI-protected storage "
                 "(mcp=%d ai=%d anthropic=%d deepseek=%d whisper=%d)",
                 migrate.McpAuthToken ? 1 : 0, migrate.AiApiKey ? 1 : 0, migrate.AiAnthropicApiKey ? 1 : 0,
                 migrate.AiDeepSeekApiKey ? 1 : 0, migrate.WhisperApiKey ? 1 : 0);
#else
        LOG_INFO("ConfigManager: migrating legacy plaintext secret(s) to DPAPI-protected storage "
                 "(mcp=%d ai=%d anthropic=%d deepseek=%d)",
                 migrate.McpAuthToken ? 1 : 0, migrate.AiApiKey ? 1 : 0, migrate.AiAnthropicApiKey ? 1 : 0,
                 migrate.AiDeepSeekApiKey ? 1 : 0);
#endif
        Save(cfg);
    }
#endif

    // Env-var + CLI overrides (applied post-disk-read), then the final post-override clamps.
    ApplyOverridesAndClamps(cli, cfg);

    if (canUseCache) {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetCachedConfigRef() = cfg;
        GetHasCachedConfigRef() = true;
    }

    return cfg;
}
