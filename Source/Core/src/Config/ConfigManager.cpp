// ConfigManager — public spine of the smatchet_config.json persistence layer. As of the
// god-file-splits partition (docs/plans/god-file-splits.md) the Save/Load/Secret bodies live in
// cohesive companion TUs; this file owns the embedded default ImGui dock-layout ini + its getter,
// the table-driven scalar field registry (shared by Save and Load via config_detail), and
// SanitizeMobileNav. Filesystem/secret/lock helpers stay in ConfigManager_PathUtils.cpp; views
// persistence + CommentTemplate serializers in ConfigManager_Views.cpp; declarations in
// ConfigManager_Internal.h. The public header (Source/Core/include/ConfigManager.h) is unchanged.

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

namespace {

// Embedded default ImGui dock-layout ini. Used by WriteDefaultImGuiSettingsFile
// (defined in ConfigManager_PathUtils.cpp) via the public getter below.
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
    {"quick_create_ctx_engine_version", &TrackerConfig::QuickCreateCtxEngineVersion},
    {"quick_create_ctx_project", &TrackerConfig::QuickCreateCtxProject},
    {"quick_create_ctx_platform", &TrackerConfig::QuickCreateCtxPlatform},
    {"quick_create_ctx_level", &TrackerConfig::QuickCreateCtxLevel},
    {"quick_create_ctx_pie_state", &TrackerConfig::QuickCreateCtxPieState},
    {"quick_create_ctx_selected_actors", &TrackerConfig::QuickCreateCtxSelectedActors},
    {"quick_create_ctx_log_tail", &TrackerConfig::QuickCreateCtxLogTail},
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
    {"mcp_tools_call_rate_burst", &TrackerConfig::McpToolsCallRateBurst},
    {"mcp_tools_call_rate_refill_per_sec", &TrackerConfig::McpToolsCallRateRefillPerSec},
    {"date_compact_relative_threshold_days", &TrackerConfig::DateCompactRelativeThresholdDays},
    {"import_max_concurrent", &TrackerConfig::ImportMaxConcurrent},
    {"grid_end_wheel_swallows_before_horizontal", &TrackerConfig::GridEndWheelSwallowsBeforeHorizontal},
    {"user_activity_day_window", &TrackerConfig::UserActivityDayWindow},
    {"max_user_changes", &TrackerConfig::MaxUserChanges},
    {"ticket_change_monitor_interval_sec", &TrackerConfig::TicketChangeMonitorIntervalSec},
    {"quick_create_ctx_log_lines", &TrackerConfig::QuickCreateCtxLogLines},
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

} // namespace

namespace smatchet {
namespace config_detail {

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

} // namespace config_detail
} // namespace smatchet

const char* ConfigManager::GetDefaultImGuiDockLayoutIni() { return kDefaultImGuiDockLayoutIni; }

// ConfigManager — Save(TrackerConfig).

// Repair a corrupt/hand-edited mobile-nav config in place. Runs on every load + after a
// Preferences Mobile-group edit so the bottom nav can never render zero pages or a home page
// with no reachable button.
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
