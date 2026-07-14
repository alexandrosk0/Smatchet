// ConfigManager::Save(TrackerConfig) + its enum/inherit field helpers and the annotate-analysis
// save. Split out of ConfigManager.cpp for the god-file-splits partition; behavior-identical body
// move. Scalar-field and secret entry points come from config_detail (ConfigManager_Internal.h).

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

using smatchet::config_detail::SaveScalarFields;
using smatchet::config_detail::SaveSecretsAndPurgeLegacy;

namespace {

// Enum -> string writes (density / panel position / theme) + ui_language normalization. Inverse
// of LoadEnumAndClampedFields's string<->enum mapping.
void SaveEnumFields(nlohmann::json& j, const TrackerConfig& config) {
    {
        const char* densityStr = "Normal";
        switch (config.Density) {
        case TrackerConfig::UiDensity::Compact:
            densityStr = "Compact";
            break;
        case TrackerConfig::UiDensity::Comfortable:
            densityStr = "Comfortable";
            break;
        default:
            break;
        }
        j["ui_density"] = densityStr;
    }
    j["panel_position"] = (config.PanelDockSide == TrackerConfig::PanelPosition::Right) ? "Right" : "Bottom";
    const char* themeStr = "ImGuiDefaultDark";
    switch (config.Theme) {
    case ThemeId::ModernDark:
        themeStr = "ModernDark";
        break;
    case ThemeId::Vs2022Dark:
        themeStr = "Vs2022Dark";
        break;
    case ThemeId::Vs2022Light:
        themeStr = "Vs2022Light";
        break;
    case ThemeId::HighContrast:
        themeStr = "HighContrast";
        break;
    case ThemeId::NortonCommander:
        themeStr = "NortonCommander";
        break;
    case ThemeId::SmatchetDark:
        themeStr = "SmatchetDark";
        break;
    case ThemeId::ImGuiDefaultDark:
    default:
        themeStr = "ImGuiDefaultDark";
        break;
    }
    j["theme"] = themeStr;
    j["uiMode"] = uiModeToString(config.UiMode);
    j["mobileNav"] = config.MobileNavPages;
    j["mobileHome"] = config.MobileHomePage;
    j["mobileDensity"] = mobileTouchDensityToString(config.MobileTouchDensity);
    j["ui_language"] = ConfigManager::NormalizeUiLanguageCode(config.UiLanguage);
}

// The three inherit-id lists (each filtered to drop the implicit "summary" entry) + the
// one-shot migration marker. Inverse of LoadInheritFieldIds / LoadListFields.
void SaveInheritFieldIds(nlohmann::json& j, const TrackerConfig& config) {
    const std::vector<std::string>* lists[4] = {&config.NewIssueInheritFieldIds, &config.NewIssueInheritFieldIdsPlane,
                                                &config.NewIssueInheritFieldIdsGitHub,
                                                &config.NewIssueInheritFieldIdsLinear};
    const char* keys[4] = {"new_issue_inherit_field_ids", "new_issue_inherit_field_ids_plane",
                           "new_issue_inherit_field_ids_github", "new_issue_inherit_field_ids_linear"};
    for (int i = 0; i < 4; ++i) {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(lists[i]->begin(), lists[i]->end(), std::back_inserter(inheritIds),
                     [](const std::string& id) { return id != "summary"; });
        j[keys[i]] = std::move(inheritIds);
    }
    j["migrated_inherit_issuetype_v1"] = config.MigratedInheritIssueTypeV1;
}

} // namespace

void ConfigManager::Save(const TrackerConfig& config) {
    // Serialize the whole read-modify-write so a concurrent writer (the config-save worker, or
    // SaveAnnotateAnalysis on another thread) can't lose-update. Distinct from GetIoMutexRef that
    // WriteConfigJson holds internally — no recursive-lock deadlock.
    std::lock_guard<std::mutex> rmwLock(GetConfigRmwMutexRef());
    nlohmann::json j = LoadMergedConfigJson();

    // Table-driven plain scalar fields (string / bool / int / float). One source-of-truth row
    // per field in kStringFields / kBoolFields / kIntFields / kFloatFields — see LoadScalarFields.
    SaveScalarFields(j, config);

    // The legacy global project scope is no longer persisted. Erase explicitly so any legacy keys
    // carried in from disk via LoadMergedConfigJson() are dropped.
    j.erase("project_key");
    j.erase("plane_project_id");
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    j["show_lua_automation_window"] = config.ShowLuaAutomationWindow;
#endif
    // Dual-key on load (`log_tracker_http_bodies` / legacy `log_jira_http_bodies`); on save only
    // the current key is written. Not table-driven for that reason.
    j["log_tracker_http_bodies"] = config.LogTrackerHttpBodies;
    // Smatchet AI assistant, Phase A prime. The legacy exploratory keys ai_model and
    // show_ai_assistant_window pre-date the provider-pluggable schema and are erased here; the
    // new provider-aware keys are written below.
    j.erase("ai_model");
    j.erase("show_ai_assistant_window");
    j["ai_provider_kind"] = config.AiProviderKind;
    j["assistant_history_max_rows"] = config.AssistantHistoryMaxRows;
#if defined(SMATCHET_WITH_WHISPER)
    // Whisper dictation — Phase A schema (additive). Non-secret fields write here; WhisperApiKey
    // is DPAPI-encrypted in SaveSecretsAndPurgeLegacy.
    j["whisper_enabled"] = config.WhisperEnabled;
    j["whisper_setup_completed"] = config.WhisperSetupCompleted;
    j["whisper_setup_choice"] = config.WhisperSetupChoice;
    j["whisper_mode"] = config.WhisperMode;
    j["whisper_model"] = config.WhisperModel;
    j["whisper_hotkey"] = config.WhisperHotkey;
    j["whisper_consent_timestamp_sec"] = config.WhisperConsentTimestampSec;
    // Phase F — language / trim / max-clip / auto-send. All additive; no schema bump.
    j["whisper_language"] = config.WhisperLanguage;
    j["whisper_trim"] = config.WhisperTrim;
    j["whisper_max_clip_sec"] = config.WhisperMaxClipSec;
    j["whisper_auto_send_on_punctuation"] = config.WhisperAutoSendOnPunctuation;
#endif
    j["mcp_export_fields"] = config.McpExportFields;
    j["quick_comment_templates"] = config.QuickCommentTemplates;
    // Lua script consent gate. Serialized by hand (path + sha-256 objects) so the pure
    // LuaScriptConsent.h stays nlohmann-free.
    {
        nlohmann::json approvals = nlohmann::json::array();
        for (const auto& a : config.ApprovedLuaScripts) {
            approvals.push_back(nlohmann::json{{"path", a.Path}, {"sha256", a.Sha256}});
        }
        j["approved_lua_scripts"] = approvals;
    }
    j["lua_script_consent_initialized"] = config.LuaScriptConsentInitialized;
    j["lua_script_consent_enforced"] = config.LuaScriptConsentEnforced;
    j["toolbar"] = config.Toolbar;
    j["keybindings"] = config.Keybindings;
    j["migrated_bugreport_hotkey_v1"] = config.MigratedBugReportHotkeyV1;
    j["migrated_menu_shortcuts_v1"] = config.MigratedMenuShortcutsV1;
    j["migrated_menu_shortcuts_v2"] = config.MigratedMenuShortcutsV2;
    j["migrated_quick_create_hotkey_v1"] = config.MigratedQuickCreateHotkeyV1;
    j["annotate_comment_templates"] = config.AnnotateCommentTemplates;
    j["duration_suggestions"] = config.DurationSuggestions;
    j["worklog_comment_templates"] = config.WorkLogCommentTemplates;
    j["layout_schema_version"] = config.LayoutSchemaVersion;
    j["font_size_pt"] = config.FontSizePt;

    SaveEnumFields(j, config);

    j.erase("mcp_server_window_layout_valid");
    j.erase("mcp_server_window_x");
    j.erase("mcp_server_window_y");
    j.erase("mcp_server_window_w");
    j.erase("mcp_server_window_h");

    SaveInheritFieldIds(j, config);
    SaveSecretsAndPurgeLegacy(j, config);
    WriteConfigJson(j);

    // Invalidate the cache only after the new file has been written, still under the
    // read-modify-write lock. Clearing the flag before the write left a window in which a racing
    // Load could re-populate the cache from the pre-save file and leave that stale entry marked
    // valid after the new file landed, so every later Load returned old data.
    std::lock_guard<std::mutex> cacheLock(GetCacheMutexRef());
    GetHasCachedConfigRef() = false;
}

void ConfigManager::SaveAnnotateAnalysis(const AnnotateAnalysisConfig& b) {
    // Serialize the read-modify-write (see ConfigManager::Save for rationale + lock-order note).
    std::lock_guard<std::mutex> rmwLock(GetConfigRmwMutexRef());
    nlohmann::json j = LoadMergedConfigJson();
    nlohmann::json ba = nlohmann::json::object();
    ba["p4_exe"] = b.P4Executable;
    ba["p4vc_exe"] = b.P4VcExecutable;
    ba["timelapse_cmd"] = b.TimelapseCommandTemplate;
    ba["change_cmd"] = b.ChangeCommandTemplate;
    ba["ai_chat_url"] = b.AiChatUrl;
    ba["default_max_frames"] = b.DefaultMaxFrames;
    ba["cl_cache_max"] = b.ChangelistCacheMaxEntries;
    ba["show_raw_callstack"] = b.ShowRawCallstack;
    ba["callstack_jira_field_id"] = b.CallstackTrackerFieldId;
    ba["last_found_cl_jira_field_id"] = b.LastFoundClTrackerFieldId;
    ba["last_occurrences_jira_field_id"] = b.LastOccurrencesTrackerFieldId;
    ba["default_ignore_keywords"] = nlohmann::json::array();
    for (const auto& kw : b.DefaultIgnoreKeywords) {
        ba["default_ignore_keywords"].push_back(kw);
    }
    ba["path_remaps"] = nlohmann::json::array();
    for (const auto& r : b.PathRemaps) {
        ba["path_remaps"].push_back(nlohmann::json{{"from", r.FromPrefix}, {"to", r.ToPrefix}});
    }
    nlohmann::json uc = nlohmann::json::object();
    auto putRgba = [&uc](const char* key, const float* v) {
        uc[key] = nlohmann::json::array({v[0], v[1], v[2], v[3]});
    };
    putRgba("status_info", b.UiColors.StatusInfo);
    putRgba("status_error", b.UiColors.StatusError);
    putRgba("status_warning", b.UiColors.StatusWarning);
    putRgba("find_highlight", b.UiColors.FindHighlight);
    putRgba("text_disabled", b.UiColors.TextDisabled);
    putRgba("import_existing", b.UiColors.ImportExisting);
    putRgba("cl_tooltip_title", b.UiColors.ClTooltipTitle);
    ba["ui_colors"] = std::move(uc);
    j["annotate_analysis"] = std::move(ba);
    WriteConfigJson(j);
}
