// ConfigManager — `Save(TrackerConfig)` / `Load(CliOverrides)` plus the annotate-analysis
// persistence pair and the embedded default ImGui dock-layout ini.
// As of `docs/plans/shipped/large-files-and-phase-2.md` § A3 the filesystem / secret / lock
// helpers and the path-and-IO half of the public surface live in
// `ConfigManager_PathUtils.cpp`; views persistence + the `CommentTemplate` ADL
// serializers live in `ConfigManager_Views.cpp`. Helper declarations are in
// `ConfigManager_Internal.h`. The public header (`Source/Core/include/ConfigManager.h`)
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
using smatchet::config_detail::GetConfigRmwMutexRef;
using smatchet::config_detail::GetHasCachedConfigRef;
using smatchet::config_detail::SanitizeConfigStringValue;

#if defined(_WIN32) || defined(__ANDROID__)
using smatchet::config_detail::ProtectSecretForConfig;
using smatchet::config_detail::UnprotectSecretFieldFromConfig;
#endif

// Embedded default ImGui dock-layout ini. Used by WriteDefaultImGuiSettingsFile
// (defined in ConfigManager_PathUtils.cpp) via the public getter below.

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
    "DockId=0x00000008,0\n"
    "\n"
    "[Window][Smatchet Assistant]\n"
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
    "[Window][Annotate###AnnotateAnalysisModal]\n"
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

// -------- field-registration tables -------------------------------------------------
// Load/Save were hand-written parallel read/write blocks (one read line plus one write line per
// config field). That parallel duplication was the whole reason both methods blew past the
// function-size caps and the recurring bug class (a field added to Save but not Load, or vice
// versa). These typed descriptor tables collapse the bulk of the plain scalar fields into one
// source-of-truth row per field; the table loops in LoadScalarFields and SaveScalarFields walk
// them so Load and Save can never drift for a table-driven field.
// Fields that need special handling (DPAPI secrets, enums, clamps, dual-key fallbacks,
// nested objects, lists, migrations) are NOT in the table — they stay as explicit code in
// the small helpers below, called from Load and Save. C++14: member-pointer rows only, no
// std::variant / structured bindings.

template <typename T> struct FieldDesc {
    const char* key;
    T TrackerConfig::*member;
};

// Plain string / bool / int fields: `cfg.member = j.value(key, cfg.member)` on Load,
// `j[key] = cfg.member` on Save. The default on Load is the member's construct-time value,
// matching the prior hand-written `j.value("k", cfg.X)` form exactly.
// NOTE: `db_path` is intentionally NOT in this table. Load reads it (explicitly, in
// LoadScalarFields) but Save must NOT persist it — DbPath is an env/CLI override-only field
// (SMATCHET_DB_PATH / --db-path); persisting it would write ephemeral override values to disk,
// breaking the "overrides take effect this process only" contract. The original hand-written
// Save() omitted it for exactly this reason; keeping it table-symmetric would have regressed that.
const FieldDesc<std::string> kStringFields[] = {
    {"domain", &TrackerConfig::Domain},
    {"email", &TrackerConfig::Email},
    {"tracker_type", &TrackerConfig::TrackerType},
    {"plane_url", &TrackerConfig::PlaneUrl},
    {"plane_workspace_slug", &TrackerConfig::PlaneWorkspaceSlug},
    {"github_base_url", &TrackerConfig::GitHubBaseUrl},
    {"github_owner", &TrackerConfig::GitHubOwner},
    {"github_repo", &TrackerConfig::GitHubRepo},
    {"linear_base_url", &TrackerConfig::LinearBaseUrl},
    {"linear_team_id", &TrackerConfig::LinearTeamId},
    {"linear_team_key", &TrackerConfig::LinearTeamKey},
    {"linear_workspace_url", &TrackerConfig::LinearWorkspaceUrl},
    {"bugreport_relay_url", &TrackerConfig::BugReportRelayUrl},
    {"bugreport_relay_key", &TrackerConfig::BugReportRelayKey},
    {"bugreport_github_owner", &TrackerConfig::BugReportGitHubOwner},
    {"bugreport_github_repo", &TrackerConfig::BugReportGitHubRepo},
    {"bugreport_github_base_url", &TrackerConfig::BugReportGitHubBaseUrl},
    {"bugreport_assets_repo", &TrackerConfig::BugReportAssetsRepo},
    {"bugreport_hotkey", &TrackerConfig::BugReportHotkey},
    {"jql", &TrackerConfig::JqlQuery},
    {"log_min_level", &TrackerConfig::LogMinLevel},
    {"ai_ollama_base_url", &TrackerConfig::AiOllamaBaseUrl},
    {"ai_base_url", &TrackerConfig::AiBaseUrl},
    {"ai_model_openai", &TrackerConfig::AiModelOpenAi},
    {"ai_model_anthropic", &TrackerConfig::AiModelAnthropic},
    {"ai_model_ollama", &TrackerConfig::AiModelOllama},
    {"ai_deepseek_base_url", &TrackerConfig::AiDeepSeekBaseUrl},
    {"ai_model_deepseek", &TrackerConfig::AiModelDeepSeek},
    {"ai_reasoning_effort", &TrackerConfig::AiReasoningEffort},
    {"agents_md_global_path", &TrackerConfig::AgentsMdGlobalPath},
    {"project_agents_md_path", &TrackerConfig::ProjectAgentsMdPath},
    {"default_issue_type_id", &TrackerConfig::DefaultIssueTypeId},
    {"default_issue_type_name", &TrackerConfig::DefaultIssueTypeName},
    {"last_import_directory", &TrackerConfig::LastImportDirectory},
    {"last_export_directory", &TrackerConfig::LastExportDirectory},
    {"date_format_option", &TrackerConfig::DateFormatOption},
    {"selected_font_name", &TrackerConfig::SelectedFontName},
    {"update_skip_version", &TrackerConfig::UpdateSkipVersion},
    {"update_github_repo", &TrackerConfig::UpdateGithubRepo},
    {"production_group_keyword", &TrackerConfig::ProductionGroupKeyword},
    {"git_commit_repos", &TrackerConfig::GitCommitRepos},
    {"vcs_feed_layout", &TrackerConfig::VcsFeedLayout},
};

const FieldDesc<bool> kBoolFields[] = {
    {"bugreport_persist_pat", &TrackerConfig::BugReportPersistPat},
    {"bugreport_hotkey_enabled", &TrackerConfig::BugReportHotkeyEnabled},
    {"bugreport_screenshot_default", &TrackerConfig::BugReportScreenshotDefault},
    {"field_overflow_tooltips", &TrackerConfig::EnableFieldOverflowTooltips},
    {"single_click_to_edit_grid_cells", &TrackerConfig::SingleClickToEditGridCells},
    {"default_long_text_editor_preview", &TrackerConfig::DefaultLongTextEditorPreview},
    {"read_only_mode", &TrackerConfig::ReadOnlyMode},
    {"backend_has_been_reachable", &TrackerConfig::BackendHasBeenReachable},
    {"window_maximized", &TrackerConfig::WindowMaximized},
    {"show_preferences_window", &TrackerConfig::ShowPreferencesWindow},
    {"show_views_dashboard_window", &TrackerConfig::ShowViewsDashboardWindow},
    {"show_performance_window", &TrackerConfig::ShowPerformanceWindow},
    {"show_log_window", &TrackerConfig::ShowLogWindow},
    {"vsync_enabled", &TrackerConfig::VsyncEnabled},
    {"log_p4_io", &TrackerConfig::LogP4Io},
    {"mcp_enabled", &TrackerConfig::McpEnabled},
    {"mcp_allow_remote", &TrackerConfig::McpAllowRemote},
    {"mcp_allow_lua_execution", &TrackerConfig::McpAllowLuaExecution},
    {"mcp_require_token_on_loopback", &TrackerConfig::McpRequireTokenOnLoopback},
    {"show_mcp_server_window", &TrackerConfig::ShowMcpServerWindow},
    {"annotate_allow_custom_commands", &TrackerConfig::AnnotateAllowCustomCommands},
    {"assistant_panel_open", &TrackerConfig::AssistantPanelOpen},
    {"assistant_panel_on_secondary_side", &TrackerConfig::AssistantPanelOnSecondarySide},
    {"agents_md_auto_discover_project", &TrackerConfig::AgentsMdAutoDiscoverProject},
    {"assistant_context_block_selection", &TrackerConfig::AssistantContextBlockSelection},
    {"assistant_context_block_visible_rows", &TrackerConfig::AssistantContextBlockVisibleRows},
    {"assistant_context_block_active_ticket", &TrackerConfig::AssistantContextBlockActiveTicket},
    {"assistant_context_block_active_view", &TrackerConfig::AssistantContextBlockActiveView},
    {"assistant_context_block_audit_trail", &TrackerConfig::AssistantContextBlockAuditTrail},
    {"assistant_outbound_consent_shown", &TrackerConfig::AssistantOutboundConsentShown},
    {"ai_prefs_verify_on_save", &TrackerConfig::AiPrefsVerifyOnSave},
    {"ai_allow_custom_endpoint_openai", &TrackerConfig::AiAllowCustomEndpointOpenAi},
    {"ai_allow_custom_endpoint_anthropic", &TrackerConfig::AiAllowCustomEndpointAnthropic},
    {"show_primary_side_bar", &TrackerConfig::ShowPrimarySideBar},
    {"show_secondary_side_bar", &TrackerConfig::ShowSecondarySideBar},
    {"show_panel", &TrackerConfig::ShowPanel},
    {"show_status_bar", &TrackerConfig::ShowStatusBar},
    {"primary_side_bar_on_right", &TrackerConfig::PrimarySideBarOnRight},
    {"update_check_enabled", &TrackerConfig::UpdateCheckEnabled},
    {"update_include_prerelease", &TrackerConfig::UpdateIncludePrerelease},
    {"ticket_change_monitor_enabled", &TrackerConfig::TicketChangeMonitorEnabled},
};

// Plain ints. Fields with a post-read clamp (ImportMaxConcurrent, GridEndWheel..., McpPort,
// DateCompactRelativeThresholdDays) read through this table; their clamp stays explicit code.
const FieldDesc<int> kIntFields[] = {
    {"window_x", &TrackerConfig::WindowX},
    {"window_y", &TrackerConfig::WindowY},
    {"window_w", &TrackerConfig::WindowWidth},
    {"window_h", &TrackerConfig::WindowHeight},
    {"field_catalog_cache_max_projects", &TrackerConfig::FieldCatalogCacheMaxProjects},
    {"hidden_pane_resident_cap", &TrackerConfig::HiddenPaneResidentCap},
    {"mcp_port", &TrackerConfig::McpPort},
    {"date_compact_relative_threshold_days", &TrackerConfig::DateCompactRelativeThresholdDays},
    {"import_max_concurrent", &TrackerConfig::ImportMaxConcurrent},
    {"grid_end_wheel_swallows_before_horizontal", &TrackerConfig::GridEndWheelSwallowsBeforeHorizontal},
    {"user_activity_day_window", &TrackerConfig::UserActivityDayWindow},
    {"max_user_changes", &TrackerConfig::MaxUserChanges},
    {"ticket_change_monitor_interval_sec", &TrackerConfig::TicketChangeMonitorIntervalSec},
};

// Floats persist as JSON doubles; read via `static_cast<float>(j.value(key, double(member)))`
// to match the prior hand-written widening/narrowing exactly.
const FieldDesc<float> kFloatFields[] = {
    {"mcp_server_info_panel_height_px", &TrackerConfig::McpServerInfoPanelHeightPx},
    {"mcp_server_activity_panel_height_px", &TrackerConfig::McpServerActivityPanelHeightPx},
    {"assistant_panel_width", &TrackerConfig::AssistantPanelWidth},
    {"view_field_picker_height", &TrackerConfig::ViewFieldPickerHeight},
    {"views_sidebar_width", &TrackerConfig::ViewsSidebarWidth},
    {"views_fields_split_ratio", &TrackerConfig::ViewsFieldsSplitRatio},
};

template <typename T, std::size_t N> std::size_t CountOf(const T (&)[N]) { return N; }

// Table-driven plain-scalar save: `j[key] = cfg.member` for every row, the exact inverse of
// LoadScalarFields. Floats serialise via the member value directly (nlohmann stores float as
// a JSON number) — symmetric with the `static_cast<float>(...double...)` widening on load.
void SaveScalarFields(nlohmann::json& j, const TrackerConfig& config) {
    for (std::size_t i = 0; i < CountOf(kStringFields); ++i) {
        j[kStringFields[i].key] = config.*(kStringFields[i].member);
    }
    // Defense-in-depth (security backlog 2026-05-17): the three header-bound base-URL fields are
    // spliced into outbound HTTP requests, so strip CR/LF/NUL before they hit disk. These fields
    // are written only via the Preferences UI -> Save; the config.set / Lua direct-write paths
    // (WriteConfigJson) cannot reach them — they are absent from the config.set allowlist
    // (ConfigSetKeyTable), not funneled through Save. NB: adding a header-bound field to that
    // allowlist later would reintroduce a header-smuggling bypass unless it also routes through
    // this sanitize. Overwrites the table-driven write just above with the sanitized value.
    j["ai_base_url"] = SanitizeConfigStringValue(config.AiBaseUrl);
    j["ai_ollama_base_url"] = SanitizeConfigStringValue(config.AiOllamaBaseUrl);
    j["ai_deepseek_base_url"] = SanitizeConfigStringValue(config.AiDeepSeekBaseUrl);
    for (std::size_t i = 0; i < CountOf(kBoolFields); ++i) {
        j[kBoolFields[i].key] = config.*(kBoolFields[i].member);
    }
    for (std::size_t i = 0; i < CountOf(kIntFields); ++i) {
        j[kIntFields[i].key] = config.*(kIntFields[i].member);
    }
    for (std::size_t i = 0; i < CountOf(kFloatFields); ++i) {
        j[kFloatFields[i].key] = config.*(kFloatFields[i].member);
    }
}

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

// Purge legacy keys carried over from the deleted SMATCHET_WITH_AGENTIC config block.
// Save() merges over existing on-disk JSON via LoadMergedConfigJson(); without explicit
// j.erase() the agentic-era secrets + settings would persist indefinitely.
void PurgeLegacyAgenticKeys(nlohmann::json& j) {
    j.erase("github_pat");
    j.erase("github_pat_enc");
    j.erase("linear_api_key");
    j.erase("linear_api_key_enc");
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
}

// WriteSecretFields is defined once per platform — the #if/#elif/#else below compiles exactly one
// definition. Splitting the platform switch across three separate function bodies (rather than one
// function with the switch inside it) keeps each arm under the function-length lint cap; the per-arm
// secret/erase contract is unchanged. See each arm's inline notes.
#if defined(_WIN32)
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    j.erase("token");
    j.erase("plane_api_key");
    j["token_enc"] = ProtectSecretForConfig(config.ApiToken);
    j["plane_api_key_enc"] = ProtectSecretForConfig(config.PlaneApiKey);
    // GitHub PAT — same plaintext-fallback pattern as McpAuthToken / AiApiKey / WhisperApiKey
    // below. Only erase the plaintext `github_pat` when DPAPI protection succeeded; otherwise
    // keep the plaintext so the user doesn't lose the only copy.
    const std::string githubPatEnc = ProtectSecretForConfig(config.GitHubPat);
    if (config.GitHubPat.empty() || !githubPatEnc.empty()) {
        j.erase("github_pat");
    }
    j["github_pat_enc"] = githubPatEnc;
    // Linear API key — same DPAPI + plaintext-fallback pattern as GitHubPat above.
    const std::string linearApiKeyEnc = ProtectSecretForConfig(config.LinearApiKey);
    if (config.LinearApiKey.empty() || !linearApiKeyEnc.empty()) {
        j.erase("linear_api_key");
    }
    j["linear_api_key_enc"] = linearApiKeyEnc;
    // Bug-report PAT — persisted ONLY when the user opted in (BugReportPersistPat);
    // secret, so DPAPI-encrypted with a plaintext fallback if protection fails.
    // When opt-out, both keys are erased so no token is ever written to config.
    j.erase("bugreport_github_pat");
    j.erase("bugreport_github_pat_enc");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        const std::string bugPatEnc = ProtectSecretForConfig(config.BugReportGitHubPat);
        if (!bugPatEnc.empty()) {
            j["bugreport_github_pat_enc"] = bugPatEnc;
        } else {
            j["bugreport_github_pat"] = config.BugReportGitHubPat; // DPAPI failed — keep plaintext fallback
        }
    }
    // Defense-in-depth (security backlog 2026-05-17): strip CR/LF/NUL from the header-bound secrets
    // (MCP auth token, AI API keys) before DPAPI-encrypting them, so the value can never carry
    // header-smuggling control chars once decrypted. Written only via Preferences UI -> Save; the
    // config.set / Lua direct-write paths cannot reach these fields (absent from the config.set
    // allowlist ConfigSetKeyTable, not funneled through Save) — any future allowlist addition of a
    // header-bound field must also route through this sanitize.
    const std::string mcpAuthTokenSanitized = SanitizeConfigStringValue(config.McpAuthToken);
    const std::string mcpAuthTokenEnc = ProtectSecretForConfig(mcpAuthTokenSanitized);
    // New field migration: keep the legacy plaintext fallback if DPAPI fails instead of dropping the only copy.
    if (mcpAuthTokenSanitized.empty() || !mcpAuthTokenEnc.empty()) {
        j.erase("mcp_auth_token");
    }
    j["mcp_auth_token_enc"] = mcpAuthTokenEnc;
    // Smatchet Assistant API keys — same DPAPI + legacy-plaintext fallback pattern as McpAuthToken.
    const std::string aiApiKeySanitized = SanitizeConfigStringValue(config.AiApiKey);
    const std::string aiApiKeyEnc = ProtectSecretForConfig(aiApiKeySanitized);
    if (aiApiKeySanitized.empty() || !aiApiKeyEnc.empty()) {
        j.erase("ai_api_key");
    }
    j["ai_api_key_enc"] = aiApiKeyEnc;
    const std::string aiAnthropicSanitized = SanitizeConfigStringValue(config.AiAnthropicApiKey);
    const std::string aiAnthropicEnc = ProtectSecretForConfig(aiAnthropicSanitized);
    if (aiAnthropicSanitized.empty() || !aiAnthropicEnc.empty()) {
        j.erase("ai_anthropic_api_key");
    }
    j["ai_anthropic_api_key_enc"] = aiAnthropicEnc;
    const std::string aiDeepSeekSanitized = SanitizeConfigStringValue(config.AiDeepSeekApiKey);
    const std::string aiDeepSeekEnc = ProtectSecretForConfig(aiDeepSeekSanitized);
    if (aiDeepSeekSanitized.empty() || !aiDeepSeekEnc.empty()) {
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
}
#elif defined(__ANDROID__)
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    // SECURITY (audit H2 / CR #1357): Android seals EVERY secret at rest through the host
    // AndroidKeyStore AES-GCM provider. ProtectSecretForConfig routes to that provider and FAILS
    // CLOSED — it returns empty when no provider is wired or the JNI seal fails. Unlike the Win32
    // DPAPI arm above there is NO plaintext fallback: an Android profile file is not a reliable
    // owner-only boundary the way a chmod-0600 desktop file is, so a secret we cannot seal is
    // DROPPED rather than written cleartext. Every plaintext key is erased unconditionally; only a
    // non-empty Keystore ciphertext is persisted.
    const auto sealSecret = [&j](const char* plainKey, const char* encKey, const std::string& value) {
        j.erase(plainKey);
        const std::string enc = ProtectSecretForConfig(value);
        if (!enc.empty()) {
            j[encKey] = enc;
        } else {
            j.erase(encKey); // empty input, or fail-closed Keystore miss — drop, never cleartext.
        }
    };
    sealSecret("token", "token_enc", config.ApiToken);
    sealSecret("plane_api_key", "plane_api_key_enc", config.PlaneApiKey);
    sealSecret("github_pat", "github_pat_enc", config.GitHubPat);
    sealSecret("linear_api_key", "linear_api_key_enc", config.LinearApiKey);
    // Bug-report PAT — persisted only on opt-in; otherwise both keys stay erased.
    j.erase("bugreport_github_pat");
    j.erase("bugreport_github_pat_enc");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        const std::string bugPatEnc = ProtectSecretForConfig(config.BugReportGitHubPat);
        if (!bugPatEnc.empty()) {
            j["bugreport_github_pat_enc"] = bugPatEnc;
        }
    }
    // Header-bound secrets: strip CR/LF/NUL before sealing (mirrors the Win32 arm).
    sealSecret("mcp_auth_token", "mcp_auth_token_enc", SanitizeConfigStringValue(config.McpAuthToken));
    sealSecret("ai_api_key", "ai_api_key_enc", SanitizeConfigStringValue(config.AiApiKey));
    sealSecret("ai_anthropic_api_key", "ai_anthropic_api_key_enc", SanitizeConfigStringValue(config.AiAnthropicApiKey));
    sealSecret("ai_deepseek_api_key", "ai_deepseek_api_key_enc", SanitizeConfigStringValue(config.AiDeepSeekApiKey));
#if defined(SMATCHET_WITH_WHISPER)
    sealSecret("whisper_api_key", "whisper_api_key_enc", config.WhisperApiKey);
#endif
}
#else
void WriteSecretFields(nlohmann::json& j, const TrackerConfig& config) {
    j.erase("token_enc");
    j.erase("plane_api_key_enc");
    j.erase("github_pat_enc");
    j.erase("linear_api_key_enc");
    j.erase("bugreport_github_pat_enc");
    j.erase("mcp_auth_token_enc");
    j.erase("ai_api_key_enc");
    j.erase("ai_anthropic_api_key_enc");
    j.erase("ai_deepseek_api_key_enc");
    j["token"] = config.ApiToken;
    j["plane_api_key"] = config.PlaneApiKey;
    j["github_pat"] = config.GitHubPat;
    j["linear_api_key"] = config.LinearApiKey;
    j.erase("bugreport_github_pat");
    if (config.BugReportPersistPat && !config.BugReportGitHubPat.empty()) {
        j["bugreport_github_pat"] = config.BugReportGitHubPat;
    }
    // Defense-in-depth (security backlog 2026-05-17): strip CR/LF/NUL from the header-bound secrets
    // before they land as plaintext JSON, so the value can never carry header-smuggling control
    // chars (mirrors the DPAPI path above — see its note re: the config.set allowlist).
    j["mcp_auth_token"] = SanitizeConfigStringValue(config.McpAuthToken);
    j["ai_api_key"] = SanitizeConfigStringValue(config.AiApiKey);
    j["ai_anthropic_api_key"] = SanitizeConfigStringValue(config.AiAnthropicApiKey);
    j["ai_deepseek_api_key"] = SanitizeConfigStringValue(config.AiDeepSeekApiKey);
#if defined(SMATCHET_WITH_WHISPER)
    j.erase("whisper_api_key_enc");
    j["whisper_api_key"] = config.WhisperApiKey;
#endif
}
#endif

// Purges legacy keys carried over from LoadMergedConfigJson() and writes the secret fields
// (DPAPI-encrypted on Win32 with a plaintext fallback when protection fails; plaintext on other
// platforms). One source-of-truth for the full secret/erase contract — see the inline notes.
void SaveSecretsAndPurgeLegacy(nlohmann::json& j, const TrackerConfig& config) {
    PurgeLegacyAgenticKeys(j);
    WriteSecretFields(j, config);
}

} // namespace

const char* ConfigManager::GetDefaultImGuiDockLayoutIni() { return kDefaultImGuiDockLayoutIni; }

// ConfigManager — Save(TrackerConfig).

void ConfigManager::Save(const TrackerConfig& config) {
    {
        std::lock_guard<std::mutex> lock(GetCacheMutexRef());
        GetHasCachedConfigRef() = false;
    }
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
    j["toolbar"] = config.Toolbar;
    j["keybindings"] = config.Keybindings;
    j["migrated_bugreport_hotkey_v1"] = config.MigratedBugReportHotkeyV1;
    j["migrated_menu_shortcuts_v1"] = config.MigratedMenuShortcutsV1;
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
}

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

// Repair a corrupt/hand-edited mobile-nav config in place. Defined outside the
// anonymous namespace below since it is a ConfigManager member. Runs on every
// load + after a Preferences Mobile-group edit so the bottom nav can never
// render zero pages or a home page with no reachable button.
void ConfigManager::SanitizeMobileNav(TrackerConfig& cfg) {
    static const char* const kKnownPages[] = {"grid", "views", "log", "settings", "ai"};
    const auto isKnown = [](const std::string& id) {
        for (const char* k : kKnownPages) {
            if (id == k) {
                return true;
            }
        }
        return false;
    };
    // Drop unknown ids + dedup (keep first occurrence's order).
    std::vector<std::string> cleaned;
    for (const std::string& id : cfg.MobileNavPages) {
        if (isKnown(id) && std::find(cleaned.begin(), cleaned.end(), id) == cleaned.end()) {
            cleaned.push_back(id);
        }
    }
    // Empty (all-unknown / hand-cleared) → restore the full default order (>=1 visible).
    if (cleaned.empty()) {
        cleaned.assign({"grid", "views", "log", "settings", "ai"});
    }
    // Home page must be a known id; an unknown value degrades to the grid home.
    if (!isKnown(cfg.MobileHomePage)) {
        cfg.MobileHomePage = "grid";
    }
    // Force the home page present so the shell always opens on a nav-reachable page.
    if (std::find(cleaned.begin(), cleaned.end(), cfg.MobileHomePage) == cleaned.end()) {
        cleaned.insert(cleaned.begin(), cfg.MobileHomePage);
    }
    cfg.MobileNavPages = std::move(cleaned);
}

// ConfigManager — Load(CliOverrides) helper functions.
// Each helper owns one cohesive slice of the former Load monolith. They are free functions in
// the anonymous namespace (no ConfigManager state beyond the json and the cfg being built),
// keeping every helper — and Load itself — under the function-size caps.

namespace {

// Flags raised when a secret is read from a legacy plaintext key (no `*_enc` present /
// undecryptable). Load() re-Saves once when any flag is set so the next on-disk state holds
// the DPAPI-protected form. Mirrors the prior local `migrateLegacyPlaintext*` bools verbatim.
struct SecretMigrationFlags {
    bool McpAuthToken = false;
    bool AiApiKey = false;
    bool AiAnthropicApiKey = false;
    bool AiDeepSeekApiKey = false;
#if defined(SMATCHET_WITH_WHISPER)
    bool WhisperApiKey = false;
#endif
#if defined(__ANDROID__)
    // Android fail-closed migration (audit H2 / CR #1357): set when ANY secret fell back to a legacy
    // plaintext key — including token/plane/github/bugreport, which have no per-field flag. Forces a
    // one-shot re-Save so the plaintext is re-sealed via Keystore, or dropped fail-closed if no
    // provider is wired. The goal is to get pre-fix plaintext OFF disk on first load after upgrade.
    bool LegacyPlaintext = false;
#endif

    bool Any() const {
        bool any = McpAuthToken || AiApiKey || AiAnthropicApiKey || AiDeepSeekApiKey;
#if defined(SMATCHET_WITH_WHISPER)
        any = any || WhisperApiKey;
#endif
#if defined(__ANDROID__)
        any = any || LegacyPlaintext;
#endif
        return any;
    }
};

// Extract the lowercased host (port stripped) from a `scheme://host[:port]/...` URL, or
// "" if it has no parseable scheme://host. Inlined here rather than reusing
// smatchet::ai::pure::ExtractUrlHost: that helper lives in the AI-gated
// AiEndpointSanitize TU (pruned from the build when SMATCHET_WITH_AI=OFF), and
// ConfigManager is always-compiled + AI-independent — a cross-dep breaks the light
// (AI-off) link. The two parsers are intentionally tiny + independent.
std::string ConfigUrlHostLower(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return std::string();
    const std::size_t hostStart = scheme + 3;
    std::size_t hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos)
        hostEnd = url.size();
    std::string host = url.substr(hostStart, hostEnd - hostStart);
    const std::size_t colon = host.find(':'); // strip :port (IPv6 literals not expected here)
    if (colon != std::string::npos)
        host.erase(colon);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return host;
}

// Table-driven plain-scalar load, one source-of-truth row per field. See the kStringFields
// table for the list. Behavior is identical to the prior per-field json-value-with-default reads.
void LoadScalarFields(const nlohmann::json& j, TrackerConfig& cfg) {
    // `db_path` is read here but never persisted by Save (override-only field — see the note on
    // kStringFields). Must stay outside the round-trip table so Save can't write it back.
    cfg.DbPath = j.value("db_path", cfg.DbPath);
    for (std::size_t i = 0; i < CountOf(kStringFields); ++i) {
        cfg.*(kStringFields[i].member) = j.value(kStringFields[i].key, cfg.*(kStringFields[i].member));
    }
    for (std::size_t i = 0; i < CountOf(kBoolFields); ++i) {
        cfg.*(kBoolFields[i].member) = j.value(kBoolFields[i].key, cfg.*(kBoolFields[i].member));
    }
    // Migration grandfather for AI custom-endpoint SSRF consent. A config written by
    // a pre-feature build carries neither consent key. If such a config already has
    // a non-canonical custom AiBaseUrl (the field both OpenAi + Anthropic read), the
    // user deliberately configured a proxy before this gate existed — auto-grant
    // consent for both so the upgrade does not silently fall back to the provider
    // default and break their setup. A post-migration config has the keys present
    // the bool loop above honours them, so a fresh repoint is NOT grandfathered.
    if (!j.contains("ai_allow_custom_endpoint_openai") && !j.contains("ai_allow_custom_endpoint_anthropic")) {
        // Compare the parsed HOST exactly — a substring match would treat a proxy
        // such as https://api.openai.com.proxy.corp as canonical and fail to
        // grandfather it, silently breaking that user's upgrade.
        const std::string host = ConfigUrlHostLower(cfg.AiBaseUrl);
        const bool hasCustomHost = !host.empty() && host != "api.openai.com" && host != "api.anthropic.com";
        if (hasCustomHost) {
            cfg.AiAllowCustomEndpointOpenAi = true;
            cfg.AiAllowCustomEndpointAnthropic = true;
        }
    }
    for (std::size_t i = 0; i < CountOf(kIntFields); ++i) {
        cfg.*(kIntFields[i].member) = j.value(kIntFields[i].key, cfg.*(kIntFields[i].member));
    }
    for (std::size_t i = 0; i < CountOf(kFloatFields); ++i) {
        cfg.*(kFloatFields[i].member) =
            static_cast<float>(j.value(kFloatFields[i].key, static_cast<double>(cfg.*(kFloatFields[i].member))));
    }
    // Dual-key fallback (current `log_tracker_http_bodies`, legacy `log_jira_http_bodies`) —
    // not table-expressible; kept explicit.
    cfg.LogTrackerHttpBodies =
        j.value("log_tracker_http_bodies", j.value("log_jira_http_bodies", cfg.LogTrackerHttpBodies));
}

// All DPAPI-encrypted secret fields with their legacy-plaintext fallback + migration flagging.
void LoadSecretFields(const nlohmann::json& j, TrackerConfig& cfg, SecretMigrationFlags& migrate) {
    (void)migrate;
#if defined(_WIN32)
    cfg.ApiToken = UnprotectSecretFieldFromConfig("token_enc", j.value("token_enc", std::string{}));
    if (cfg.ApiToken.empty()) {
        cfg.ApiToken = j.value("token", std::string{});
    }
    cfg.PlaneApiKey = UnprotectSecretFieldFromConfig("plane_api_key_enc", j.value("plane_api_key_enc", std::string{}));
    if (cfg.PlaneApiKey.empty()) {
        cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
    }
    // GitHub PAT: same DPAPI + legacy-plaintext shape as PlaneApiKey.
    cfg.GitHubPat = UnprotectSecretFieldFromConfig("github_pat_enc", j.value("github_pat_enc", std::string{}));
    if (cfg.GitHubPat.empty()) {
        cfg.GitHubPat = j.value("github_pat", std::string{});
    }
    // Linear API key — same DPAPI + legacy-plaintext shape as GitHubPat.
    cfg.LinearApiKey =
        UnprotectSecretFieldFromConfig("linear_api_key_enc", j.value("linear_api_key_enc", std::string{}));
    if (cfg.LinearApiKey.empty()) {
        cfg.LinearApiKey = j.value("linear_api_key", std::string{});
    }
    // Bug-report PAT — DPAPI + legacy-plaintext, same shape as GitHubPat.
    cfg.BugReportGitHubPat =
        UnprotectSecretFieldFromConfig("bugreport_github_pat_enc", j.value("bugreport_github_pat_enc", std::string{}));
    if (cfg.BugReportGitHubPat.empty()) {
        cfg.BugReportGitHubPat = j.value("bugreport_github_pat", std::string{});
    }
    cfg.McpAuthToken =
        UnprotectSecretFieldFromConfig("mcp_auth_token_enc", j.value("mcp_auth_token_enc", std::string{}));
    if (cfg.McpAuthToken.empty()) {
        cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
        migrate.McpAuthToken = !cfg.McpAuthToken.empty();
    }
    cfg.AiApiKey = UnprotectSecretFieldFromConfig("ai_api_key_enc", j.value("ai_api_key_enc", std::string{}));
    if (cfg.AiApiKey.empty()) {
        cfg.AiApiKey = j.value("ai_api_key", std::string{});
        migrate.AiApiKey = !cfg.AiApiKey.empty();
    }
    cfg.AiAnthropicApiKey =
        UnprotectSecretFieldFromConfig("ai_anthropic_api_key_enc", j.value("ai_anthropic_api_key_enc", std::string{}));
    if (cfg.AiAnthropicApiKey.empty()) {
        cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
        migrate.AiAnthropicApiKey = !cfg.AiAnthropicApiKey.empty();
    }
    cfg.AiDeepSeekApiKey =
        UnprotectSecretFieldFromConfig("ai_deepseek_api_key_enc", j.value("ai_deepseek_api_key_enc", std::string{}));
    if (cfg.AiDeepSeekApiKey.empty()) {
        cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
        migrate.AiDeepSeekApiKey = !cfg.AiDeepSeekApiKey.empty();
    }
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey =
        UnprotectSecretFieldFromConfig("whisper_api_key_enc", j.value("whisper_api_key_enc", std::string{}));
    if (cfg.WhisperApiKey.empty()) {
        cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
        migrate.WhisperApiKey = !cfg.WhisperApiKey.empty();
    }
#endif
#elif defined(__ANDROID__)
    // SECURITY (audit H2 / CR #1357): unseal every secret through the host Keystore provider.
    // UnprotectSecretFieldFromConfig -> UnprotectSecretFromConfig FAILS SAFE to empty (no provider, a
    // Keystore/JNI decrypt failure, or ciphertext minted on another device). Plaintext fallback is gated
    // on the SEALED key being ABSENT — not on an empty unseal: a present-but-undecryptable `*_enc`
    // (tamper, key rotation, foreign-device ciphertext) is DROPPED, never downgraded to a sibling
    // plaintext an attacker could have injected. We fall back to legacy plaintext only when no sealed key
    // exists (a pre-Keystore config), flagging a migration so Load() re-Saves — re-sealing via Keystore,
    // or dropping fail-closed if no provider is wired. Either way pre-fix plaintext leaves disk on load.
    const auto unsealSecret = [&j, &migrate](const char* encKey, const char* plainKey) -> std::string {
        const std::string sealed = j.value(encKey, std::string{});
        if (!sealed.empty()) {
            // Sealed key present: trust ONLY a successful unseal. A failed unseal that coexists with a
            // plaintext sibling still flags migration (purge on re-Save) but never surfaces it.
            const std::string value = UnprotectSecretFieldFromConfig(encKey, sealed);
            if (value.empty() && !j.value(plainKey, std::string{}).empty()) {
                migrate.LegacyPlaintext = true; // purge stale plaintext on re-Save; do not trust it.
            }
            return value;
        }
        std::string value = j.value(plainKey, std::string{});
        if (!value.empty()) {
            migrate.LegacyPlaintext = true; // legacy plaintext, no sealed key — re-Save to seal/drop it.
        }
        return value;
    };
    cfg.ApiToken = unsealSecret("token_enc", "token");
    cfg.PlaneApiKey = unsealSecret("plane_api_key_enc", "plane_api_key");
    cfg.GitHubPat = unsealSecret("github_pat_enc", "github_pat");
    cfg.LinearApiKey = unsealSecret("linear_api_key_enc", "linear_api_key");
    cfg.BugReportGitHubPat = unsealSecret("bugreport_github_pat_enc", "bugreport_github_pat");
    cfg.McpAuthToken = unsealSecret("mcp_auth_token_enc", "mcp_auth_token");
    cfg.AiApiKey = unsealSecret("ai_api_key_enc", "ai_api_key");
    cfg.AiAnthropicApiKey = unsealSecret("ai_anthropic_api_key_enc", "ai_anthropic_api_key");
    cfg.AiDeepSeekApiKey = unsealSecret("ai_deepseek_api_key_enc", "ai_deepseek_api_key");
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey = unsealSecret("whisper_api_key_enc", "whisper_api_key");
#endif
#else
    cfg.ApiToken = j.value("token", std::string{});
    cfg.PlaneApiKey = j.value("plane_api_key", std::string{});
    cfg.GitHubPat = j.value("github_pat", std::string{});
    cfg.LinearApiKey = j.value("linear_api_key", std::string{});
    cfg.BugReportGitHubPat = j.value("bugreport_github_pat", std::string{});
    cfg.McpAuthToken = j.value("mcp_auth_token", std::string{});
    cfg.AiApiKey = j.value("ai_api_key", std::string{});
    cfg.AiAnthropicApiKey = j.value("ai_anthropic_api_key", std::string{});
    cfg.AiDeepSeekApiKey = j.value("ai_deepseek_api_key", std::string{});
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperApiKey = j.value("whisper_api_key", std::string{});
#endif
#endif
}

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
        try {
            LoadScalarFields(j, cfg);
            LoadSecretFields(j, cfg, migrate);
            LoadEnumAndClampedFields(j, cfg);
#if defined(SMATCHET_WITH_WHISPER)
            LoadWhisperFields(j, cfg);
#endif
            LoadListFields(j, cfg);
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
