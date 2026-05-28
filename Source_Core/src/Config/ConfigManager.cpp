// ConfigManager — `Save(TrackerConfig)` / `Load(CliOverrides)` plus the blame-analysis
// persistence pair and the embedded default ImGui dock-layout ini.
//
// As of `docs/design/large-files-and-phase-2.md` § A3 the filesystem / secret / lock
// helpers and the path-and-IO half of the public surface live in
// `ConfigManager_PathUtils.cpp`; views persistence + the `CommentTemplate` ADL
// serializers live in `ConfigManager_Views.cpp`. Helper declarations are in
// `ConfigManager_Internal.h`. The public header (`Source_Core/include/ConfigManager.h`)
// is unchanged.

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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>

using smatchet::config_detail::GetCachedConfigRef;
using smatchet::config_detail::GetCacheMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;

#if defined(_WIN32)
using smatchet::config_detail::ProtectSecretForConfig;
using smatchet::config_detail::UnprotectSecretFieldFromConfig;
#endif

// ---------------------------------------------------------------------------
// Embedded default ImGui dock-layout ini. Used by WriteDefaultImGuiSettingsFile
// (defined in ConfigManager_PathUtils.cpp) via the public getter below.
// ---------------------------------------------------------------------------

namespace {

// Verbatim copy of the user-verified working ini at
// %LOCALAPPDATA%/Smatchet/imgui.ini. This is the live runtime layout the user
// has settled on; it survives fresh launches + resize without panel eviction.
// Update this constant by copying the live imgui.ini after manual layout tweaks.
constexpr char kDefaultImGuiDockLayoutIni[] =
    "[Window][WindowOverViewport_11111111]\n"
    "Pos=0,22\n"
    "Size=1920,965\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Debug##Default]\n"
    "Pos=60,60\n"
    "Size=400,400\n"
    "Collapsed=0\n"
    "\n"
    "[Window][Preferences]\n"
    "Pos=0,510\n"
    "Size=1920,477\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,0\n"
    "\n"
    "[Window][Smatchet - Active Project]\n"
    "Pos=0,22\n"
    "Size=1045,486\n"
    "Collapsed=0\n"
    "DockId=0x00000002,0\n"
    "\n"
    "[Window][Views - Jira]\n"
    "Pos=926,22\n"
    "Size=354,698\n"
    "Collapsed=0\n"
    "DockId=0x00000008,0\n"
    "\n"
    "[Window][Performance]\n"
    "Pos=0,510\n"
    "Size=1920,477\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,7\n"
    "\n"
    "[Window][SmatchetViewsDashboard]\n"
    "Pos=1047,22\n"
    "Size=873,486\n"
    "Collapsed=0\n"
    "DockId=0x00000004,0\n"
    "\n"
    "[Window][Log]\n"
    "Pos=0,510\n"
    "Size=1920,477\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,1\n"
    "\n"
    "[Window][Backend Audit]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,2\n"
    "\n"
    "[Window][Scripting]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,3\n"
    "\n"
    "[Window][MCP Server]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,4\n"
    "\n"
    "[Window][Bulk import tickets]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,6\n"
    "\n"
    "[Window][Bulk export tickets]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,5\n"
    "\n"
    "[Window][Annotate###BlameAnalysisModal]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,8\n"
    "\n"
    "[Window][Plan docs]\n"
    "Pos=0,559\n"
    "Size=1920,450\n"
    "Collapsed=0\n"
    "DockId=0x0000000A,9\n"
    "\n"
    "[Table][0x518B645F,7]\n"
    "Column 0  Weight=1.0000\n"
    "Column 1  Weight=1.0000\n"
    "Column 2  Weight=1.0000 Sort=0^\n"
    "Column 3  Weight=1.0000\n"
    "Column 4  Weight=1.0000\n"
    "Column 5  Weight=1.0000\n"
    "Column 6  Weight=1.0000\n"
    "\n"
    "[Table][0x4BC636F5,8]\n"
    "RefScale=16\n"
    "\n"
    "[Table][0x70C366DB,5]\n"
    "RefScale=16\n"
    "Column 0  Width=48\n"
    "Column 1  Width=180\n"
    "Column 2  Weight=1.0000\n"
    "Column 3  Width=120\n"
    "Column 4  Width=220\n"
    "\n"
    "[Docking][Data]\n"
    "DockSpace         ID=0x08BD597D Window=0x1BBC0F80 Pos=0,22 Size=1920,965 Split=Y Selected=0x7EBEC904\n"
    "  DockNode        ID=0x00000009 Parent=0x08BD597D SizeRef=1920,486 Split=X\n"
    "    DockNode      ID=0x00000001 Parent=0x00000009 SizeRef=1045,987 Split=X\n"
    "      DockNode    ID=0x00000002 Parent=0x00000001 SizeRef=700,987 CentralNode=1 HiddenTabBar=1 "
    "Selected=0x7EBEC904\n"
    "      DockNode    ID=0x00000008 Parent=0x00000001 SizeRef=354,987 Selected=0x51577D15\n"
    "    DockNode      ID=0x00000004 Parent=0x00000009 SizeRef=873,987 Selected=0x74648FC6\n"
    "  DockNode        ID=0x0000000A Parent=0x08BD597D SizeRef=1920,477 Selected=0x6A4695A4\n";

} // namespace

const char* ConfigManager::GetDefaultImGuiDockLayoutIni() { return kDefaultImGuiDockLayoutIni; }

// ===========================================================================
// ConfigManager — Save(TrackerConfig).
// ===========================================================================

void ConfigManager::Save(const TrackerConfig& config) {
    {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetHasCachedConfigRef() = false;
    }
    nlohmann::json j = LoadMergedConfigJson();
    j["domain"] = config.Domain;
    j["email"] = config.Email;
    // PR 5 of docs/design/archive/remove-global-project-key.md: stop persisting the legacy global
    // project scope. The fields are still on `TrackerConfig` until PR 6 deletes them, but the
    // on-disk JSON no longer round-trips them — older builds reading the new config will see
    // them missing and default to empty, matching the new code path. Erase explicitly so any
    // legacy keys carried in from disk via `LoadMergedConfigJson()` are dropped.
    j.erase("project_key");
    j.erase("plane_project_id");
    j["tracker_type"] = config.TrackerType;
    j["plane_url"] = config.PlaneUrl;
    j["plane_workspace_slug"] = config.PlaneWorkspaceSlug;
    // GitHub-as-tracker fields (PR2 of docs/design/archive/github-tracker-backend.md).
    // PAT goes through DPAPI same as plane_api_key — see saveGithubPat block below.
    j["github_base_url"] = config.GitHubBaseUrl;
    j["github_owner"] = config.GitHubOwner;
    j["github_repo"] = config.GitHubRepo;
    j["jql"] = config.JqlQuery;
    j["field_overflow_tooltips"] = config.EnableFieldOverflowTooltips;
    j["single_click_to_edit_grid_cells"] = config.SingleClickToEditGridCells;
    j["read_only_mode"] = config.ReadOnlyMode;
    j["backend_has_been_reachable"] = config.BackendHasBeenReachable;
    j["grid_end_wheel_swallows_before_horizontal"] = config.GridEndWheelSwallowsBeforeHorizontal;
    j["window_x"] = config.WindowX;
    j["window_y"] = config.WindowY;
    j["window_w"] = config.WindowWidth;
    j["window_h"] = config.WindowHeight;
    j["window_maximized"] = config.WindowMaximized;
    j["field_catalog_cache_max_projects"] = config.FieldCatalogCacheMaxProjects;
    j["show_preferences_window"] = config.ShowPreferencesWindow;
    j["show_views_dashboard_window"] = config.ShowViewsDashboardWindow;
    j["show_performance_window"] = config.ShowPerformanceWindow;
    j["show_log_window"] = config.ShowLogWindow;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    j["show_lua_automation_window"] = config.ShowLuaAutomationWindow;
#endif
    j["log_min_level"] = config.LogMinLevel;
    j["log_tracker_http_bodies"] = config.LogTrackerHttpBodies;
    j["log_p4_io"] = config.LogP4Io;
    // Smatchet Assistant (AI) — Phase A'. Legacy exploratory keys (`ai_model`, `show_ai_assistant_window`)
    // pre-date the provider-pluggable schema and are erased; the new provider-aware keys are written below.
    j.erase("ai_model");
    j.erase("show_ai_assistant_window");
    j["ai_provider_kind"] = config.AiProviderKind;
    j["ai_ollama_base_url"] = config.AiOllamaBaseUrl;
    j["ai_base_url"] = config.AiBaseUrl;
    j["ai_model_openai"] = config.AiModelOpenAi;
    j["ai_model_anthropic"] = config.AiModelAnthropic;
    j["ai_model_ollama"] = config.AiModelOllama;
    // DeepSeek — model + base URL serialise verbatim; the API key is handled
    // alongside the other AI secrets in the DPAPI block below.
    j["ai_deepseek_base_url"] = config.AiDeepSeekBaseUrl;
    j["ai_model_deepseek"] = config.AiModelDeepSeek;
    j["ai_reasoning_effort"] = config.AiReasoningEffort;
    j["assistant_panel_open"] = config.AssistantPanelOpen;
    j["assistant_panel_width"] = config.AssistantPanelWidth;
    j["assistant_panel_on_secondary_side"] = config.AssistantPanelOnSecondarySide;
    j["agents_md_global_path"] = config.AgentsMdGlobalPath;
    j["project_agents_md_path"] = config.ProjectAgentsMdPath;
    j["agents_md_auto_discover_project"] = config.AgentsMdAutoDiscoverProject;
    j["assistant_context_block_selection"] = config.AssistantContextBlockSelection;
    j["assistant_context_block_visible_rows"] = config.AssistantContextBlockVisibleRows;
    j["assistant_context_block_active_ticket"] = config.AssistantContextBlockActiveTicket;
    j["assistant_context_block_active_view"] = config.AssistantContextBlockActiveView;
    j["assistant_context_block_audit_trail"] = config.AssistantContextBlockAuditTrail;
    j["assistant_history_max_rows"] = config.AssistantHistoryMaxRows;
    j["ai_prefs_verify_on_save"] = config.AiPrefsVerifyOnSave;
#if defined(SMATCHET_WITH_WHISPER)
    // Whisper dictation — Phase A schema (additive). Non-secret fields write
    // here; WhisperApiKey is DPAPI-encrypted alongside AiApiKey below.
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
    j["mcp_enabled"] = config.McpEnabled;
    j["mcp_port"] = config.McpPort;
    j["mcp_allow_remote"] = config.McpAllowRemote;
    j["mcp_allow_lua_execution"] = config.McpAllowLuaExecution;
    j["mcp_export_fields"] = config.McpExportFields;
    j["show_mcp_server_window"] = config.ShowMcpServerWindow;
    j["quick_comment_templates"] = config.QuickCommentTemplates;
    j["blame_comment_templates"] = config.BlameCommentTemplates;
    j["duration_suggestions"] = config.DurationSuggestions;
    j["worklog_comment_templates"] = config.WorkLogCommentTemplates;
    j["date_format_option"] = config.DateFormatOption;
    j["date_compact_relative_threshold_days"] = config.DateCompactRelativeThresholdDays;
    j["view_field_picker_height"] = config.ViewFieldPickerHeight;
    j["views_sidebar_width"] = config.ViewsSidebarWidth;
    j["views_fields_split_ratio"] = config.ViewsFieldsSplitRatio;
    j["selected_font_name"] = config.SelectedFontName;
    j["show_primary_side_bar"] = config.ShowPrimarySideBar;
    j["show_secondary_side_bar"] = config.ShowSecondarySideBar;
    j["show_panel"] = config.ShowPanel;
    j["show_status_bar"] = config.ShowStatusBar;
    j["layout_schema_version"] = config.LayoutSchemaVersion;
    j["font_size_pt"] = config.FontSizePt;
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
    {
        j["panel_position"] = (config.PanelDockSide == TrackerConfig::PanelPosition::Right) ? "Right" : "Bottom";
    }
    j["primary_side_bar_on_right"] = config.PrimarySideBarOnRight;
    auto themeToString = [](ThemeId t) -> const char* {
        switch (t) {
        case ThemeId::ModernDark:
            return "ModernDark";
        case ThemeId::Vs2022Dark:
            return "Vs2022Dark";
        case ThemeId::Vs2022Light:
            return "Vs2022Light";
        case ThemeId::HighContrast:
            return "HighContrast";
        case ThemeId::NortonCommander:
            return "NortonCommander";
        case ThemeId::ImGuiDefaultDark:
            return "ImGuiDefaultDark";
        case ThemeId::SmatchetDark:
            return "SmatchetDark";
        default:
            return "ImGuiDefaultDark";
        }
    };
    j["theme"] = themeToString(config.Theme);
    j["ui_language"] = NormalizeUiLanguageCode(config.UiLanguage);
    j["update_check_enabled"] = config.UpdateCheckEnabled;
    j["update_include_prerelease"] = config.UpdateIncludePrerelease;
    j["update_skip_version"] = config.UpdateSkipVersion;
    j["update_github_repo"] = config.UpdateGithubRepo;
    j.erase("mcp_server_window_layout_valid");
    j.erase("mcp_server_window_x");
    j.erase("mcp_server_window_y");
    j.erase("mcp_server_window_w");
    j.erase("mcp_server_window_h");
    j["mcp_server_info_panel_height_px"] = config.McpServerInfoPanelHeightPx;
    j["mcp_server_activity_panel_height_px"] = config.McpServerActivityPanelHeightPx;
    j["blame_allow_custom_commands"] = config.BlameAllowCustomCommands;
    j["default_issue_type_id"] = config.DefaultIssueTypeId;
    j["default_issue_type_name"] = config.DefaultIssueTypeName;
    j["import_max_concurrent"] = config.ImportMaxConcurrent;
    j["last_import_directory"] = config.LastImportDirectory;
    j["last_export_directory"] = config.LastExportDirectory;
    {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(config.NewIssueInheritFieldIds.begin(), config.NewIssueInheritFieldIds.end(),
                     std::back_inserter(inheritIds), [](const std::string& id) { return id != "summary"; });
        j["new_issue_inherit_field_ids"] = std::move(inheritIds);
    }
    {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(config.NewIssueInheritFieldIdsPlane.begin(), config.NewIssueInheritFieldIdsPlane.end(),
                     std::back_inserter(inheritIds), [](const std::string& id) { return id != "summary"; });
        j["new_issue_inherit_field_ids_plane"] = std::move(inheritIds);
    }
    {
        nlohmann::json inheritIds = nlohmann::json::array();
        std::copy_if(config.NewIssueInheritFieldIdsGitHub.begin(), config.NewIssueInheritFieldIdsGitHub.end(),
                     std::back_inserter(inheritIds), [](const std::string& id) { return id != "summary"; });
        j["new_issue_inherit_field_ids_github"] = std::move(inheritIds);
    }
    j["migrated_inherit_issuetype_v1"] = config.MigratedInheritIssueTypeV1;
    // Purge legacy keys carried over from the deleted SMATCHET_WITH_AGENTIC config block.
    // Save() merges over existing on-disk JSON via LoadMergedConfigJson(); without explicit
    // j.erase() the agentic-era secrets + settings would persist indefinitely. Per
    // CodeRabbit review on PR #356 (2026-05-21). Covers the GitHub PAT pair, the agentic-poll
    // tunables, handoff knobs, coderabbit-react / ci-react nested objects.
    j.erase("github_pat");
    j.erase("github_pat_enc");
    j.erase("agentic_poll_enabled");
    j.erase("agentic_poll_interval_sec");
    j.erase("agentic_poll_source");
    j.erase("agentic_poll_query");
    j.erase("handoff_harness_bin_path");
    j.erase("handoff_runner_name");
    j.erase("handoff_clarification_post_to_github");
    j.erase("handoff_auto_create_pr_if_missing");
    j.erase("handoff_pr_base_branch");
    j.erase("handoff_pr_body_template");
    j.erase("handoff_git_bin_path");
    j.erase("handoff_gh_bin_path");
    j.erase("handoff_pr_iteration_budget");
    j.erase("handoff_pr_comment_poll_interval_sec");
    j.erase("handoff_auto_start_on_approve");
    j.erase("coderabbit_react");
    j.erase("ci_react");
#if defined(_WIN32)
    j.erase("token");
    j.erase("plane_api_key");
    j["token_enc"] = ProtectSecretForConfig(config.ApiToken);
    j["plane_api_key_enc"] = ProtectSecretForConfig(config.PlaneApiKey);
    // GitHub PAT — same plaintext-fallback pattern as McpAuthToken / AiApiKey / WhisperApiKey
    // below. Only erase the plaintext `github_pat` when DPAPI protection succeeded; otherwise
    // keep the plaintext so the user doesn't lose the only copy. Per CodeRabbit finding on
    // PR #357 (HEAD `1a3ed3318d` review): the prior unconditional `j.erase("github_pat")` +
    // `j["github_pat_enc"] = ProtectSecretForConfig(...)` ordering would delete the plaintext
    // even on a failed protector call, leaving the user with neither plaintext nor encrypted.
    const std::string githubPatEnc = ProtectSecretForConfig(config.GitHubPat);
    if (config.GitHubPat.empty() || !githubPatEnc.empty()) {
        j.erase("github_pat");
    }
    j["github_pat_enc"] = githubPatEnc;
    const std::string mcpAuthTokenEnc = ProtectSecretForConfig(config.McpAuthToken);
    // New field migration: keep the legacy plaintext fallback if DPAPI fails instead of dropping the only copy.
    if (config.McpAuthToken.empty() || !mcpAuthTokenEnc.empty()) {
        j.erase("mcp_auth_token");
    }
    j["mcp_auth_token_enc"] = mcpAuthTokenEnc;
    // Smatchet Assistant API keys — same DPAPI + legacy-plaintext fallback pattern as McpAuthToken.
    const std::string aiApiKeyEnc = ProtectSecretForConfig(config.AiApiKey);
    if (config.AiApiKey.empty() || !aiApiKeyEnc.empty()) {
        j.erase("ai_api_key");
    }
    j["ai_api_key_enc"] = aiApiKeyEnc;
    const std::string aiAnthropicEnc = ProtectSecretForConfig(config.AiAnthropicApiKey);
    if (config.AiAnthropicApiKey.empty() || !aiAnthropicEnc.empty()) {
        j.erase("ai_anthropic_api_key");
    }
    j["ai_anthropic_api_key_enc"] = aiAnthropicEnc;
    const std::string aiDeepSeekEnc = ProtectSecretForConfig(config.AiDeepSeekApiKey);
    if (config.AiDeepSeekApiKey.empty() || !aiDeepSeekEnc.empty()) {
        j.erase("ai_deepseek_api_key");
    }
    j["ai_deepseek_api_key_enc"] = aiDeepSeekEnc;
#if defined(SMATCHET_WITH_WHISPER)
    // WhisperApiKey — same DPAPI + legacy-plaintext fallback shape as AiApiKey.
    const std::string whisperApiKeyEnc = ProtectSecretForConfig(config.WhisperApiKey);
    if (config.WhisperApiKey.empty() || !whisperApiKeyEnc.empty()) {
        j.erase("whisper_api_key");
    }
    j["whisper_api_key_enc"] = whisperApiKeyEnc;
#endif
#else
    j.erase("token_enc");
    j.erase("plane_api_key_enc");
    j.erase("github_pat_enc");
    j.erase("mcp_auth_token_enc");
    j.erase("ai_api_key_enc");
    j.erase("ai_anthropic_api_key_enc");
    j.erase("ai_deepseek_api_key_enc");
    j["token"] = config.ApiToken;
    j["plane_api_key"] = config.PlaneApiKey;
    j["github_pat"] = config.GitHubPat;
    j["mcp_auth_token"] = config.McpAuthToken;
    j["ai_api_key"] = config.AiApiKey;
    j["ai_anthropic_api_key"] = config.AiAnthropicApiKey;
    j["ai_deepseek_api_key"] = config.AiDeepSeekApiKey;
#if defined(SMATCHET_WITH_WHISPER)
    j.erase("whisper_api_key_enc");
    j["whisper_api_key"] = config.WhisperApiKey;
#endif
#endif
    WriteConfigJson(j);
}

// ===========================================================================
// ConfigManager — blame-analysis persistence.
// ===========================================================================

BlameAnalysisConfig ConfigManager::LoadBlameAnalysis() {
    nlohmann::json j = LoadMergedConfigJson();
    BlameAnalysisConfig b;
    if (!j.contains("blame_analysis") || !j["blame_analysis"].is_object()) {
        return b;
    }
    const nlohmann::json& ba = j["blame_analysis"];
    b.P4Executable = ba.value("p4_exe", b.P4Executable);
    b.P4VcExecutable = ba.value("p4vc_exe", b.P4VcExecutable);
    b.TimelapseCommandTemplate = ba.value("timelapse_cmd", std::string());
    b.ChangeCommandTemplate = ba.value("change_cmd", std::string());
    b.AiChatUrl = ba.value("ai_chat_url", std::string());
    b.DefaultMaxFrames = ba.value("default_max_frames", b.DefaultMaxFrames);
    b.ChangelistCacheMaxEntries = ba.value("cl_cache_max", b.ChangelistCacheMaxEntries);
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

void ConfigManager::SaveBlameAnalysis(const BlameAnalysisConfig& b) {
    nlohmann::json j = LoadMergedConfigJson();
    nlohmann::json ba = nlohmann::json::object();
    ba["p4_exe"] = b.P4Executable;
    ba["p4vc_exe"] = b.P4VcExecutable;
    ba["timelapse_cmd"] = b.TimelapseCommandTemplate;
    ba["change_cmd"] = b.ChangeCommandTemplate;
    ba["ai_chat_url"] = b.AiChatUrl;
    ba["default_max_frames"] = b.DefaultMaxFrames;
    ba["cl_cache_max"] = b.ChangelistCacheMaxEntries;
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
    j["blame_analysis"] = std::move(ba);
    WriteConfigJson(j);
}

// ===========================================================================
// ConfigManager — Load(CliOverrides).
// ===========================================================================

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
#if defined(_WIN32)
    bool migrateLegacyPlaintextMcpAuthToken = false;
    bool migrateLegacyPlaintextAiApiKey = false;
    bool migrateLegacyPlaintextAiAnthropicApiKey = false;
    bool migrateLegacyPlaintextAiDeepSeekApiKey = false;
#if defined(SMATCHET_WITH_WHISPER)
    // Mirror of the AiApiKey migration shape — when Load() falls
    // back to plaintext `whisper_api_key` (because `whisper_api_key_enc` was
    // absent or undecryptable), flag for re-save so the next on-disk state
    // holds the value under `whisper_api_key_enc` and removes the plaintext.
    bool migrateLegacyPlaintextWhisperApiKey = false;
#endif
#endif

    if (!j.empty()) {
        try {
            cfg.DbPath = j.value("db_path", cfg.DbPath);
            cfg.Domain = j.value("domain", std::string{});
            cfg.Email = j.value("email", std::string{});
#if defined(_WIN32)
            cfg.ApiToken = UnprotectSecretFieldFromConfig("token_enc", j.value("token_enc", std::string{}));
            if (cfg.ApiToken.empty()) {
                cfg.ApiToken = j.value("token", std::string{});
            }
#else
            cfg.ApiToken = j.value("token", std::string{});
#endif
            // PR 7: legacy `project_key` / `plane_project_id` migration carriers removed.
            // The one-shot sweeps still run but with empty values; the cache_meta marker
            // (`legacy_project_swept_v1`) ensures they no-op on installs that already migrated.
            cfg.TrackerType = j.value("tracker_type", cfg.TrackerType);
            cfg.PlaneUrl = j.value("plane_url", cfg.PlaneUrl);
            cfg.PlaneWorkspaceSlug = j.value("plane_workspace_slug", cfg.PlaneWorkspaceSlug);

#if defined(_WIN32)
            cfg.PlaneApiKey =
                UnprotectSecretFieldFromConfig("plane_api_key_enc", j.value("plane_api_key_enc", std::string{}));
            if (cfg.PlaneApiKey.empty()) {
                cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
            }
            // PR2 of github-tracker-backend.md — same DPAPI + legacy-plaintext shape as PlaneApiKey.
            cfg.GitHubPat = UnprotectSecretFieldFromConfig("github_pat_enc", j.value("github_pat_enc", std::string{}));
            if (cfg.GitHubPat.empty()) {
                cfg.GitHubPat = j.value("github_pat", std::string{});
            }
#else
            cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
            cfg.GitHubPat = j.value("github_pat", std::string{});
#endif
            cfg.GitHubBaseUrl = j.value("github_base_url", cfg.GitHubBaseUrl);
            cfg.GitHubOwner = j.value("github_owner", cfg.GitHubOwner);
            cfg.GitHubRepo = j.value("github_repo", cfg.GitHubRepo);

            cfg.JqlQuery = j.value("jql", cfg.JqlQuery);
            cfg.EnableFieldOverflowTooltips = j.value("field_overflow_tooltips", cfg.EnableFieldOverflowTooltips);
            cfg.SingleClickToEditGridCells = j.value("single_click_to_edit_grid_cells", cfg.SingleClickToEditGridCells);
            cfg.ReadOnlyMode = j.value("read_only_mode", cfg.ReadOnlyMode);
            cfg.BackendHasBeenReachable = j.value("backend_has_been_reachable", cfg.BackendHasBeenReachable);
            cfg.GridEndWheelSwallowsBeforeHorizontal =
                j.value("grid_end_wheel_swallows_before_horizontal", cfg.GridEndWheelSwallowsBeforeHorizontal);
            cfg.WindowX = j.value("window_x", cfg.WindowX);
            cfg.WindowY = j.value("window_y", cfg.WindowY);
            cfg.WindowWidth = j.value("window_w", cfg.WindowWidth);
            cfg.WindowHeight = j.value("window_h", cfg.WindowHeight);
            cfg.WindowMaximized = j.value("window_maximized", cfg.WindowMaximized);
            cfg.FieldCatalogCacheMaxProjects =
                j.value("field_catalog_cache_max_projects", cfg.FieldCatalogCacheMaxProjects);
            cfg.ShowPreferencesWindow = j.value("show_preferences_window", cfg.ShowPreferencesWindow);
            cfg.ShowViewsDashboardWindow = j.value("show_views_dashboard_window", cfg.ShowViewsDashboardWindow);
            cfg.ShowPerformanceWindow = j.value("show_performance_window", cfg.ShowPerformanceWindow);
            cfg.ShowLogWindow = j.value("show_log_window", cfg.ShowLogWindow);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            cfg.ShowLuaAutomationWindow = j.value("show_lua_automation_window", cfg.ShowLuaAutomationWindow);
#endif
            cfg.LogMinLevel = j.value("log_min_level", cfg.LogMinLevel);
            cfg.LogTrackerHttpBodies =
                j.value("log_tracker_http_bodies", j.value("log_jira_http_bodies", cfg.LogTrackerHttpBodies));
            cfg.LogP4Io = j.value("log_p4_io", cfg.LogP4Io);
            cfg.McpEnabled = j.value("mcp_enabled", cfg.McpEnabled);
            cfg.McpPort = j.value("mcp_port", cfg.McpPort);
            cfg.McpAllowRemote = j.value("mcp_allow_remote", cfg.McpAllowRemote);
#if defined(_WIN32)
            cfg.McpAuthToken =
                UnprotectSecretFieldFromConfig("mcp_auth_token_enc", j.value("mcp_auth_token_enc", std::string{}));
            if (cfg.McpAuthToken.empty()) {
                cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
                migrateLegacyPlaintextMcpAuthToken = !cfg.McpAuthToken.empty();
            }
#else
            cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
#endif
            cfg.McpAllowLuaExecution = j.value("mcp_allow_lua_execution", cfg.McpAllowLuaExecution);
            if (j.contains("mcp_export_fields") && j["mcp_export_fields"].is_array()) {
                for (const auto& item : j["mcp_export_fields"]) {
                    if (item.is_string()) {
                        cfg.McpExportFields.push_back(item.get<std::string>());
                    }
                }
            }
            cfg.ShowMcpServerWindow = j.value("show_mcp_server_window", cfg.ShowMcpServerWindow);
            cfg.McpServerInfoPanelHeightPx = static_cast<float>(
                j.value("mcp_server_info_panel_height_px", static_cast<double>(cfg.McpServerInfoPanelHeightPx)));
            cfg.McpServerActivityPanelHeightPx = static_cast<float>(j.value(
                "mcp_server_activity_panel_height_px", static_cast<double>(cfg.McpServerActivityPanelHeightPx)));
            cfg.BlameAllowCustomCommands = j.value("blame_allow_custom_commands", cfg.BlameAllowCustomCommands);

            // Smatchet Assistant (AI) — Phase A'. Clamp `ai_provider_kind` to the known enum range;
            // a future-version persisted int (e.g. 99) on an older build degrades to OpenAi (0)
            // rather than triggering UB on enum cast.
            {
                const int rawProvider = j.value("ai_provider_kind", cfg.AiProviderKind);
                const int kProviderMin = static_cast<int>(AiProvider::OpenAi);
                const int kProviderMax = static_cast<int>(AiProvider::DeepSeek);
                cfg.AiProviderKind = (rawProvider < kProviderMin || rawProvider > kProviderMax)
                                         ? static_cast<int>(AiProvider::OpenAi)
                                         : rawProvider;
            }
#if defined(_WIN32)
            cfg.AiApiKey = UnprotectSecretFieldFromConfig("ai_api_key_enc", j.value("ai_api_key_enc", std::string{}));
            if (cfg.AiApiKey.empty()) {
                cfg.AiApiKey = j.value("ai_api_key", std::string{});
                migrateLegacyPlaintextAiApiKey = !cfg.AiApiKey.empty();
            }
            cfg.AiAnthropicApiKey = UnprotectSecretFieldFromConfig("ai_anthropic_api_key_enc",
                                                                   j.value("ai_anthropic_api_key_enc", std::string{}));
            if (cfg.AiAnthropicApiKey.empty()) {
                cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
                migrateLegacyPlaintextAiAnthropicApiKey = !cfg.AiAnthropicApiKey.empty();
            }
            cfg.AiDeepSeekApiKey = UnprotectSecretFieldFromConfig("ai_deepseek_api_key_enc",
                                                                  j.value("ai_deepseek_api_key_enc", std::string{}));
            if (cfg.AiDeepSeekApiKey.empty()) {
                cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
                migrateLegacyPlaintextAiDeepSeekApiKey = !cfg.AiDeepSeekApiKey.empty();
            }
#if defined(SMATCHET_WITH_WHISPER)
            cfg.WhisperApiKey =
                UnprotectSecretFieldFromConfig("whisper_api_key_enc", j.value("whisper_api_key_enc", std::string{}));
            if (cfg.WhisperApiKey.empty()) {
                cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
                migrateLegacyPlaintextWhisperApiKey = !cfg.WhisperApiKey.empty();
            }
#endif
#else
            cfg.AiApiKey = j.value("ai_api_key", std::string{});
            cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
            cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
#if defined(SMATCHET_WITH_WHISPER)
            cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
#endif
#endif
#if defined(SMATCHET_WITH_WHISPER)
            // Whisper dictation — Phase A schema (additive; missing fields fall back to defaults).
            cfg.WhisperEnabled = j.value("whisper_enabled", cfg.WhisperEnabled);
            cfg.WhisperSetupCompleted = j.value("whisper_setup_completed", cfg.WhisperSetupCompleted);
            cfg.WhisperSetupChoice = j.value("whisper_setup_choice", cfg.WhisperSetupChoice);
            cfg.WhisperMode = j.value("whisper_mode", cfg.WhisperMode);
            cfg.WhisperModel = j.value("whisper_model", cfg.WhisperModel);
            cfg.WhisperHotkey = j.value("whisper_hotkey", cfg.WhisperHotkey);
            cfg.WhisperConsentTimestampSec = j.value("whisper_consent_timestamp_sec", cfg.WhisperConsentTimestampSec);
            // Phase F — language / trim / max-clip / auto-send (additive; defaults
            // survive when the field is missing from older config files).
            cfg.WhisperLanguage = j.value("whisper_language", cfg.WhisperLanguage);
            cfg.WhisperTrim = j.value("whisper_trim", cfg.WhisperTrim);
            cfg.WhisperMaxClipSec = j.value("whisper_max_clip_sec", cfg.WhisperMaxClipSec);
            cfg.WhisperAutoSendOnPunctuation =
                j.value("whisper_auto_send_on_punctuation", cfg.WhisperAutoSendOnPunctuation);
            // Clamp WhisperMaxClipSec into the documented range so a hand-edited
            // config can't drive runaway cloud cost. 0 disables; positive values
            // clamp at 600 s.
            if (cfg.WhisperMaxClipSec < 0) {
                cfg.WhisperMaxClipSec = 0;
            } else if (cfg.WhisperMaxClipSec > 600) {
                cfg.WhisperMaxClipSec = 600;
            }
#endif
            cfg.AiOllamaBaseUrl = j.value("ai_ollama_base_url", cfg.AiOllamaBaseUrl);
            cfg.AiBaseUrl = j.value("ai_base_url", cfg.AiBaseUrl);
            cfg.AiModelOpenAi = j.value("ai_model_openai", cfg.AiModelOpenAi);
            cfg.AiModelAnthropic = j.value("ai_model_anthropic", cfg.AiModelAnthropic);
            cfg.AiModelOllama = j.value("ai_model_ollama", cfg.AiModelOllama);
            // DeepSeek base URL + model — additive; older configs round-trip
            // the construct-time defaults (empty URL → built-in default
            // `https://api.deepseek.com`; model `deepseek-chat`).
            cfg.AiDeepSeekBaseUrl = j.value("ai_deepseek_base_url", cfg.AiDeepSeekBaseUrl);
            cfg.AiModelDeepSeek = j.value("ai_model_deepseek", cfg.AiModelDeepSeek);
            cfg.AiReasoningEffort = j.value("ai_reasoning_effort", cfg.AiReasoningEffort);
            cfg.AssistantPanelOpen = j.value("assistant_panel_open", cfg.AssistantPanelOpen);
            cfg.AssistantPanelWidth =
                static_cast<float>(j.value("assistant_panel_width", static_cast<double>(cfg.AssistantPanelWidth)));
            cfg.AssistantPanelOnSecondarySide =
                j.value("assistant_panel_on_secondary_side", cfg.AssistantPanelOnSecondarySide);
            cfg.AgentsMdGlobalPath = j.value("agents_md_global_path", cfg.AgentsMdGlobalPath);
            cfg.ProjectAgentsMdPath = j.value("project_agents_md_path", cfg.ProjectAgentsMdPath);
            cfg.AgentsMdAutoDiscoverProject =
                j.value("agents_md_auto_discover_project", cfg.AgentsMdAutoDiscoverProject);
            cfg.AssistantContextBlockSelection =
                j.value("assistant_context_block_selection", cfg.AssistantContextBlockSelection);
            cfg.AssistantContextBlockVisibleRows =
                j.value("assistant_context_block_visible_rows", cfg.AssistantContextBlockVisibleRows);
            cfg.AssistantContextBlockActiveTicket =
                j.value("assistant_context_block_active_ticket", cfg.AssistantContextBlockActiveTicket);
            cfg.AssistantContextBlockActiveView =
                j.value("assistant_context_block_active_view", cfg.AssistantContextBlockActiveView);
            cfg.AssistantContextBlockAuditTrail =
                j.value("assistant_context_block_audit_trail", cfg.AssistantContextBlockAuditTrail);
            // Clamp on load — negative / zero would silently disable history; a stray
            // very-large value would let the SQLite file grow without bound. Cap at
            // 100k which is comfortably above any realistic conversation depth.
            const int rawMaxRows = j.value("assistant_history_max_rows", cfg.AssistantHistoryMaxRows);
            cfg.AssistantHistoryMaxRows = (rawMaxRows < 1) ? 1 : (rawMaxRows > 100000 ? 100000 : rawMaxRows);
            cfg.AiPrefsVerifyOnSave = j.value("ai_prefs_verify_on_save", cfg.AiPrefsVerifyOnSave);

            cfg.DateFormatOption = j.value("date_format_option", cfg.DateFormatOption);
            cfg.DateCompactRelativeThresholdDays =
                j.value("date_compact_relative_threshold_days", cfg.DateCompactRelativeThresholdDays);
            if (cfg.DateCompactRelativeThresholdDays < 1)
                cfg.DateCompactRelativeThresholdDays = 1;
            if (cfg.DateCompactRelativeThresholdDays > 365)
                cfg.DateCompactRelativeThresholdDays = 365;
            cfg.DefaultIssueTypeId = j.value("default_issue_type_id", cfg.DefaultIssueTypeId);
            cfg.DefaultIssueTypeName = j.value("default_issue_type_name", cfg.DefaultIssueTypeName);
            cfg.ImportMaxConcurrent = j.value("import_max_concurrent", cfg.ImportMaxConcurrent);
            cfg.LastImportDirectory = j.value("last_import_directory", cfg.LastImportDirectory);
            cfg.LastExportDirectory = j.value("last_export_directory", cfg.LastExportDirectory);
            cfg.ViewFieldPickerHeight = j.value("view_field_picker_height", cfg.ViewFieldPickerHeight);
            cfg.ViewsSidebarWidth = j.value("views_sidebar_width", cfg.ViewsSidebarWidth);
            cfg.ViewsFieldsSplitRatio = j.value("views_fields_split_ratio", cfg.ViewsFieldsSplitRatio);
            cfg.ShowPrimarySideBar = j.value("show_primary_side_bar", cfg.ShowPrimarySideBar);
            cfg.ShowSecondarySideBar = j.value("show_secondary_side_bar", cfg.ShowSecondarySideBar);
            cfg.ShowPanel = j.value("show_panel", cfg.ShowPanel);
            cfg.ShowStatusBar = j.value("show_status_bar", cfg.ShowStatusBar);
            cfg.SelectedFontName = j.value("selected_font_name", cfg.SelectedFontName);
            cfg.LayoutSchemaVersion = j.value("layout_schema_version", 0);
            if (cfg.LayoutSchemaVersion < 0) {
                cfg.LayoutSchemaVersion = 0;
            }
            cfg.FontSizePt = j.value("font_size_pt", cfg.FontSizePt);
            if (cfg.FontSizePt < 8) {
                cfg.FontSizePt = 8;
            }
            if (cfg.FontSizePt > 32) {
                cfg.FontSizePt = 32;
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
                cfg.PanelDockSide = (panelPosStr == "Right") ? TrackerConfig::PanelPosition::Right
                                                             : TrackerConfig::PanelPosition::Bottom;
            }
            cfg.PrimarySideBarOnRight = j.value("primary_side_bar_on_right", cfg.PrimarySideBarOnRight);
            // Default-string mirrors the construct-time default in TrackerConfig::Theme — fresh
            // installs (no `theme` key on disk) land on ImGuiDefaultDark. Existing configs with a
            // serialized value round-trip back to whatever they persisted (so users on SmatchetDark
            // keep SmatchetDark across the default change).
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
            cfg.UiLanguage = NormalizeUiLanguageCode(j.value("ui_language", cfg.UiLanguage));
            cfg.UpdateCheckEnabled = j.value("update_check_enabled", cfg.UpdateCheckEnabled);
            cfg.UpdateIncludePrerelease = j.value("update_include_prerelease", cfg.UpdateIncludePrerelease);
            cfg.UpdateSkipVersion = j.value("update_skip_version", cfg.UpdateSkipVersion);
            cfg.UpdateGithubRepo = j.value("update_github_repo", cfg.UpdateGithubRepo);
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

            if (j.contains("blame_comment_templates") && j["blame_comment_templates"].is_array()) {
                cfg.BlameCommentTemplates.clear();
                for (const auto& item : j["blame_comment_templates"]) {
                    try {
                        cfg.BlameCommentTemplates.push_back(item.get<CommentTemplate>());
                    } catch (...) { // catch-all-ok: skip malformed template entry
                    }
                }
            } else {
                cfg.BlameCommentTemplates = GetDefaultBlameCommentTemplates();
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

            {
                cfg.NewIssueInheritFieldIds = DefaultNewIssueInheritFieldIdsList();
                if (j.contains("new_issue_inherit_field_ids") && j["new_issue_inherit_field_ids"].is_array()) {
                    cfg.NewIssueInheritFieldIds.clear();
                    for (const auto& item : j["new_issue_inherit_field_ids"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                                s.erase(0, 1);
                            }
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
                                s.pop_back();
                            }
                            if (!s.empty() && s != "summary") {
                                cfg.NewIssueInheritFieldIds.push_back(std::move(s));
                            }
                        }
                    }
                }
                if (cfg.NewIssueInheritFieldIds.empty()) {
                    cfg.NewIssueInheritFieldIds = DefaultNewIssueInheritFieldIdsList();
                }
            }
            {
                cfg.NewIssueInheritFieldIdsPlane = DefaultNewIssueInheritFieldIdsList();
                if (j.contains("new_issue_inherit_field_ids_plane") &&
                    j["new_issue_inherit_field_ids_plane"].is_array()) {
                    cfg.NewIssueInheritFieldIdsPlane.clear();
                    for (const auto& item : j["new_issue_inherit_field_ids_plane"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                                s.erase(0, 1);
                            }
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
                                s.pop_back();
                            }
                            if (!s.empty() && s != "summary") {
                                cfg.NewIssueInheritFieldIdsPlane.push_back(std::move(s));
                            }
                        }
                    }
                }
                if (cfg.NewIssueInheritFieldIdsPlane.empty()) {
                    cfg.NewIssueInheritFieldIdsPlane = DefaultNewIssueInheritFieldIdsList();
                }
            }
            {
                cfg.NewIssueInheritFieldIdsGitHub = DefaultNewIssueInheritFieldIdsList();
                if (j.contains("new_issue_inherit_field_ids_github") &&
                    j["new_issue_inherit_field_ids_github"].is_array()) {
                    cfg.NewIssueInheritFieldIdsGitHub.clear();
                    for (const auto& item : j["new_issue_inherit_field_ids_github"]) {
                        if (item.is_string()) {
                            std::string s = item.get<std::string>();
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                                s.erase(0, 1);
                            }
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
                                s.pop_back();
                            }
                            if (!s.empty() && s != "summary") {
                                cfg.NewIssueInheritFieldIdsGitHub.push_back(std::move(s));
                            }
                        }
                    }
                }
                if (cfg.NewIssueInheritFieldIdsGitHub.empty()) {
                    cfg.NewIssueInheritFieldIdsGitHub = DefaultNewIssueInheritFieldIdsList();
                }
            }

            // One-shot migration: inject "issuetype" into the front of all inherit lists if it
            // is absent. Older configs were persisted with the legacy default list (no issuetype),
            // so a fresh-install benefit only ships to existing users via this migration.
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
                cfg.MigratedInheritIssueTypeV1 = true;
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("ConfigManager: Load() parse error: %s", ex.what());
        } catch (...) {
            LOG_ERROR("ConfigManager: Load() parse error (unknown)");
        }
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

#if defined(_WIN32)
    // Only MCP gets an eager legacy cleanup here; older secrets keep their established lazy migration behavior.
    //
    // Ordering note (backlog #15): this Save(cfg) runs BEFORE the env/CLI override block below, by design.
    // - cfg here reflects what disk contained (with the legacy plaintext token already decoded into cfg.McpAuthToken).
    //   Save() re-encrypts McpAuthToken into mcp_auth_token_enc on disk, which is the whole point of the migration.
    // - Env / CLI overrides (DbPath, TrackerType, McpPort, McpAllowRemote) are ephemeral and must NOT be persisted —
    //   they're meant to take effect this process only. Running Save AFTER overrides would write override values
    //   back to disk and pollute the next launch when the env/CLI is no longer set.
    // - Consequence: for the rest of this Load(), in-memory cfg (and the cache filled at the bottom) hold
    //   override-applied values while disk holds pre-override values. That divergence is intentional and matches
    //   pre-split behavior. The standing limitation that any subsequent Save() with this cfg would write
    //   override values to disk is a pre-existing concern outside the scope of this migration.
    bool migrateAny = migrateLegacyPlaintextMcpAuthToken || migrateLegacyPlaintextAiApiKey ||
                      migrateLegacyPlaintextAiAnthropicApiKey || migrateLegacyPlaintextAiDeepSeekApiKey;
#if defined(SMATCHET_WITH_WHISPER)
    migrateAny = migrateAny || migrateLegacyPlaintextWhisperApiKey;
#endif
    if (migrateAny) {
#if defined(SMATCHET_WITH_WHISPER)
        LOG_INFO("ConfigManager: migrating legacy plaintext secret(s) to DPAPI-protected storage "
                 "(mcp=%d ai=%d anthropic=%d deepseek=%d whisper=%d)",
                 migrateLegacyPlaintextMcpAuthToken ? 1 : 0, migrateLegacyPlaintextAiApiKey ? 1 : 0,
                 migrateLegacyPlaintextAiAnthropicApiKey ? 1 : 0, migrateLegacyPlaintextAiDeepSeekApiKey ? 1 : 0,
                 migrateLegacyPlaintextWhisperApiKey ? 1 : 0);
#else
        LOG_INFO("ConfigManager: migrating legacy plaintext secret(s) to DPAPI-protected storage "
                 "(mcp=%d ai=%d anthropic=%d deepseek=%d)",
                 migrateLegacyPlaintextMcpAuthToken ? 1 : 0, migrateLegacyPlaintextAiApiKey ? 1 : 0,
                 migrateLegacyPlaintextAiAnthropicApiKey ? 1 : 0, migrateLegacyPlaintextAiDeepSeekApiKey ? 1 : 0);
#endif
        Save(cfg);
    }
#endif

    // Apply Option 1 overrides (Environment Variables)
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

    // SMATCHET_TRACKER_TOKEN — tracker API credential (Jira ApiToken / Plane ApiKey / GitHub PAT).
    // Never stored in argv; always passed as an env var for CI/agent runs.
    if (const char* envToken = std::getenv("SMATCHET_TRACKER_TOKEN")) {
        if (envToken[0] != '\0') {
            if (cfg.TrackerType == "Plane") {
                cfg.PlaneApiKey = envToken;
            } else if (cfg.TrackerType == "github") {
                cfg.GitHubPat = envToken;
            } else {
                cfg.ApiToken = envToken;
            }
        }
    }

    // SMATCHET_TRACKER_BASE_URL — tracker origin URL.
    // For Jira: maps to cfg.Domain (e.g. "https://company.atlassian.net").
    // For Plane: maps to cfg.PlaneUrl (e.g. "https://api.plane.so").
    // For GitHub: maps to cfg.GitHubBaseUrl (e.g. "https://api.github.com" or
    //   "https://<enterprise>/api/v3"). Per CodeRabbit finding on PR #357 — env-driven
    //   trackerType=github used to run with empty credentials because this mapper only
    //   targeted Jira/Plane.
    if (const char* envBase = std::getenv("SMATCHET_TRACKER_BASE_URL")) {
        if (envBase[0] != '\0') {
            if (cfg.TrackerType == "Plane") {
                cfg.PlaneUrl = envBase;
            } else if (cfg.TrackerType == "github") {
                cfg.GitHubBaseUrl = envBase;
            } else {
                cfg.Domain = envBase;
            }
        }
    }

    // SMATCHET_LOG_LEVEL — minimum log verbosity: trace|debug|info|warn|error.
    if (const char* envLog = std::getenv("SMATCHET_LOG_LEVEL")) {
        if (envLog[0] != '\0') {
            cfg.LogMinLevel = envLog;
        }
    }

    // Apply Option 4 overrides (CLI parameters)
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

    // Post-override clamp and safety bounds checking
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

    if (canUseCache) {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetCachedConfigRef() = cfg;
        GetHasCachedConfigRef() = true;
    }

    return cfg;
}
