#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

// Public surface of the Smatchet config persistence layer. The implementation lives in
// Source_Core/src/ConfigManager.cpp — including this header no longer pulls <windows.h>,
// <wincrypt.h>, <fstream>, <sstream>, or 1200 lines of inline method bodies. nlohmann/json
// stays because the struct serializers (CommentTemplate to_json/from_json) need it.

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "SmatchetDefaults.h"

struct CommentTemplate {
    std::string Id;
    std::string Title;
    std::string Text;

    // Support nlohmann::json serialization
    friend void to_json(nlohmann::json& j, const CommentTemplate& t) {
        j = nlohmann::json{{"id", t.Id}, {"title", t.Title}, {"text", t.Text}};
    }

    friend void from_json(const nlohmann::json& j, CommentTemplate& t) {
        t.Id = j.value("id", "");
        t.Title = j.value("title", "");
        t.Text = j.value("text", "");
    }
};

inline std::vector<CommentTemplate> GetDefaultQuickCommentTemplates() {
    return {
        {"need_repro", "Need repro details",
         "Need reproduction details for {key}:\n- Repro steps\n- Expected vs actual result\n- Branch / CL / build\n- "
         "Environment details"},
        {"need_logs", "Need logs / diagnostics",
         "Please attach diagnostic data for {key}:\n- Relevant logs\n- Callstack / crash context\n- Local repro notes"},
        {"handoff", "Triage handoff summary",
         "Triage handoff for {key}:\n- Current owner: \n- Next action: \n- ETA: \n- Blockers:"}};
}

inline std::vector<CommentTemplate> GetDefaultBlameCommentTemplates() {
    return {
        {"need_repro", "Need repro details", "Need repro details for {key} (blame context: {path}:{line}, CL {cl})."},
        {"need_logs", "Need logs / diagnostics",
         "Please attach logs/diagnostics for {key} to continue triage.\nReference: {function} @ {path}:{line}."},
        {"handoff", "Triage handoff summary",
         "Triage handoff for {key}:\n- Suggested owner: {user}\n- Suspect location: {function} ({path}:{line})\n- CL: "
         "{cl}"}};
}

struct TrackerConfig {
    std::string DbPath = SmatchetDefaults::kDefaultDbPath;
    std::string Domain;   // e.g., "yourcompany.atlassian.net"
    std::string Email;    // e.g., "dev@company.com"
    std::string ApiToken; // Your Atlassian API Token
    // Jira project key used by create meta enrichment calls (e.g. PROJ).
    std::string ProjectKey;

    // Tracker Type: "Jira" or "Plane"
    std::string TrackerType = SmatchetDefaults::kDefaultBackendType;

    // Plane.so specific configuration
    std::string PlaneUrl;           // API origin: https://api.plane.so (no path); https://app.plane.so normalized
    std::string PlaneWorkspaceSlug; // e.g. "my-workspace"
    std::string PlaneProjectId;     // UUID of the project
    std::string PlaneApiKey;        // Plane API Key

    // JQL used when querying Jira; defaults to issues assigned to the current user.
    std::string JqlQuery = "assignee=currentUser()";
    // Jira field keys to extract and cache (e.g. customfield_12345, duedate).
    std::vector<std::string> SelectedFields;
    // When true, show tooltips on hover when grid field text overflows (clipped or multiline).
    // Exposed in UI as Settings -> Preferences -> Appearance.
    bool EnableFieldOverflowTooltips = true;
    // When true, tracker-changing actions are disabled. Defaults on only for first launch with no setup config.
    bool ReadOnlyMode = false;
    // Wheel ticks at top/bottom before vertical wheel reroutes to horizontal grid scroll.
    // Exposed in UI as Settings -> Preferences -> Appearance.
    int GridEndWheelSwallowsBeforeHorizontal = 15;
    // Restores Settings -> Preferences window visibility on launch.
    bool ShowPreferencesWindow = false;
    // Restores Workspace -> Views & Queries window visibility on launch.
    bool ShowViewsDashboardWindow = true;
    // Restores Inspect -> Performance Monitor window visibility on launch.
    bool ShowPerformanceWindow = false;
    // Restores Inspect -> Runtime Log window visibility on launch.
    bool ShowLogWindow = false;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // Restores Automation -> Scripts & Actions window visibility on launch.
    bool ShowLuaAutomationWindow = false;
#endif
    // Minimum log level: trace, debug, info, warn, error (see Logger::ParseLogLevelString).
    std::string LogMinLevel = "info";
    // When true, ITrackerClient logs truncated HTTP response bodies at Trace.
    bool LogTrackerHttpBodies = false;
    // When true, P4Blame logs truncated p4 stdout at Trace (plus stderr on non-zero exit).
    bool LogP4Io = false;
    // When true, MCP plugin HTTP server is started.
    bool McpEnabled = false;
    // MCP plugin listen port.
    int McpPort = SmatchetDefaults::Mcp::kDefaultPort;
    // When false (default), bind MCP to localhost only.
    bool McpAllowRemote = false;
    // Optional shared secret required via X-Smatchet-Token header.
    std::string McpAuthToken;
    // Off by default: allow MCP clients to execute Lua code/snippets through built-in run_lua tool.
    bool McpAllowLuaExecution = false;
    // Field ids that MCP /list_tickets and /search are allowed to export.
    // Empty = safe default subset (summary, status, priority, assignee, updated, created, labels, issuetype).
    std::vector<std::string> McpExportFields;
    /** Automation -> Agent Bridge (MCP)... window open on launch (like ShowPerformanceWindow). */
    bool ShowMcpServerWindow = false;
    /** Height of the copyable status/endpoints block; 0 = use default (line height × 18). */
    float McpServerInfoPanelHeightPx = 0.f;
    /** Height of the Recent actions copyable block. */
    float McpServerActivityPanelHeightPx = 140.f;
    // Allow Blame Analysis to launch user-supplied custom `timelapse_cmd` / `change_cmd` templates.
    // Off by default: these run arbitrary programs; only enable if the config file is trusted.
    bool BlameAllowCustomCommands = false;

    // Default Jira issue type id for new-issue drafts when we can't infer one
    // from the last displayed ticket (e.g. empty grid). Jira numeric id (e.g. "10001").
    std::string DefaultIssueTypeId;
    // Display name fallback matching DefaultIssueTypeId (optional, shown in UI before first catalog fetch).
    std::string DefaultIssueTypeName;
    // Max concurrent in-flight POST /issue requests during bulk import.
    int ImportMaxConcurrent = 4;
    // Last directory picked in the bulk-import file dialog. Empty = use cwd.
    std::string LastImportDirectory;
    // Last directory picked in the bulk-export file dialog. Empty = use cwd.
    std::string LastExportDirectory;
    // Jira field ids copied from the last grid row when seeding a new-issue draft (+ New issue).
    std::vector<std::string> NewIssueInheritFieldIds;
    // Plane field ids copied from the last grid row when seeding a new-issue draft (+ New issue).
    std::vector<std::string> NewIssueInheritFieldIdsPlane;

    // Quick comment templates for context menus and blame analysis
    std::vector<CommentTemplate> QuickCommentTemplates = GetDefaultQuickCommentTemplates();
    std::vector<CommentTemplate> BlameCommentTemplates = GetDefaultBlameCommentTemplates();

    // Custom suggestions and templates saved in smatchet_config.json
    std::vector<std::string> DurationSuggestions = {"15m", "30m", "1h", "2h", "4h", "8h", "1d", "2d", "1w"};
    std::vector<std::string> WorkLogCommentTemplates = {
        "Investigated and resolved the issue.", "Tested and verified on local environment.",
        "Refactored code and ran static analysis.", "Discussed with team and updated implementation.",
        "Wrote unit tests and verified all passing."};

    // Date formatting preferences
    std::string DateFormatOption = "compact";
    int DateCompactRelativeThresholdDays = 21;

    // View Field Picker height
    float ViewFieldPickerHeight = 220.0f;

    // Font setting
    std::string SelectedFontName = "Segoe UI";
    // UI localization preference (normalized to en-US or fr-FR).
    std::string UiLanguage = "en-US";
    // Standalone updater preferences.
    bool UpdateCheckEnabled = true;
    bool UpdateIncludePrerelease = false;
    std::string UpdateSkipVersion;
    std::string UpdateGithubRepo = "alexandrosk0/Smatchet";
};

struct ViewSortSpec {
    std::string ColumnKey; // "id" or "field:status" etc.
    int Direction = 0;     // 0=None, 1=Ascending, 2=Descending (ImGuiSortDirection)
    bool operator==(const ViewSortSpec& o) const { return ColumnKey == o.ColumnKey && Direction == o.Direction; }
    bool operator!=(const ViewSortSpec& o) const { return !(*this == o); }
};

struct ViewDefinition {
    std::string Id;
    std::string Name;
    std::string Jql = "assignee=currentUser()";
    std::vector<std::string> Fields;
    std::vector<std::string> ColumnOrder;
    std::unordered_map<std::string, float> ColumnWidths;
    std::vector<ViewSortSpec> SortSpecs;
};

struct ViewsStore {
    int Version = 1;
    std::string ActiveViewId;
    std::vector<ViewDefinition> Views;
};

/** One tracker backend's saved views (disk v2 `backends` entry). */
struct ViewWorkspaceState {
    std::string ActiveViewId;
    std::vector<ViewDefinition> Views;
};

/** Full smatchet_views.json on disk (version 2 with per-backend buckets). */
struct PersistentViewsFile {
    int Version = 2;
    std::unordered_map<std::string, ViewWorkspaceState> Backends;
};

namespace SmatchetViewsDiskDetail {

inline ViewDefinition ParseViewDefinition(const nlohmann::json& viewJson) {
    ViewDefinition view;
    view.Id = viewJson.value("id", std::string());
    view.Name = viewJson.value("name", std::string());
    view.Jql = viewJson.value("jql", view.Jql);
    if (viewJson.contains("fields") && viewJson["fields"].is_array()) {
        for (const auto& field : viewJson["fields"]) {
            if (field.is_string()) {
                view.Fields.push_back(field.get<std::string>());
            }
        }
    }
    if (viewJson.contains("column_order") && viewJson["column_order"].is_array()) {
        for (const auto& col : viewJson["column_order"]) {
            if (col.is_string()) {
                view.ColumnOrder.push_back(col.get<std::string>());
            }
        }
    }
    if (viewJson.contains("column_widths") && viewJson["column_widths"].is_object()) {
        for (auto it = viewJson["column_widths"].begin(); it != viewJson["column_widths"].end(); ++it) {
            if (it.value().is_number()) {
                view.ColumnWidths[it.key()] = it.value().get<float>();
            }
        }
    }
    if (viewJson.contains("sort_specs") && viewJson["sort_specs"].is_array()) {
        for (const auto& specJson : viewJson["sort_specs"]) {
            if (specJson.is_object() && specJson.contains("column") && specJson["column"].is_string()) {
                ViewSortSpec spec;
                spec.ColumnKey = specJson["column"].get<std::string>();
                spec.Direction = specJson.value("direction", 0);
                if (spec.Direction != 0) {
                    view.SortSpecs.push_back(spec);
                }
            }
        }
    }
    if (view.Id.empty()) {
        view.Id = view.Name;
    }
    if (view.Name.empty()) {
        view.Name = view.Id.empty() ? std::string("View") : view.Id;
    }
    return view;
}

inline ViewWorkspaceState ParseWorkspaceObject(const nlohmann::json& root) {
    ViewWorkspaceState ws;
    ws.ActiveViewId = root.value("active_view_id", std::string());
    if (root.contains("views") && root["views"].is_array()) {
        for (const auto& viewJson : root["views"]) {
            if (!viewJson.is_object()) {
                continue;
            }
            ws.Views.push_back(ParseViewDefinition(viewJson));
        }
    }
    if (ws.ActiveViewId.empty() && !ws.Views.empty()) {
        ws.ActiveViewId = ws.Views.front().Id;
    }
    return ws;
}

inline nlohmann::json SerializeView(const ViewDefinition& view) {
    nlohmann::json viewJson;
    viewJson["id"] = view.Id;
    viewJson["name"] = view.Name;
    viewJson["jql"] = view.Jql;
    viewJson["fields"] = view.Fields;
    viewJson["column_order"] = view.ColumnOrder;
    viewJson["column_widths"] = nlohmann::json::object();
    for (const auto& kv : view.ColumnWidths) {
        viewJson["column_widths"][kv.first] = kv.second;
    }
    viewJson["sort_specs"] = nlohmann::json::array();
    for (const auto& spec : view.SortSpecs) {
        if (spec.Direction != 0) {
            viewJson["sort_specs"].push_back(nlohmann::json{{"column", spec.ColumnKey}, {"direction", spec.Direction}});
        }
    }
    return viewJson;
}

inline nlohmann::json SerializeWorkspace(const ViewWorkspaceState& ws) {
    nlohmann::json j = nlohmann::json::object();
    j["active_view_id"] = ws.ActiveViewId;
    j["views"] = nlohmann::json::array();
    for (const auto& view : ws.Views) {
        j["views"].push_back(SerializeView(view));
    }
    return j;
}

inline ViewWorkspaceState MakeDefaultViewWorkspaceForBackend(const std::string& backendKey, const TrackerConfig& cfg) {
    ViewWorkspaceState ws;
    if (backendKey == "Plane") {
        ViewDefinition v;
        v.Id = "plane_default_view";
        v.Name = "Default Plane View";
        v.Jql = "";
        v.Fields = {"summary", "status", "priority", "assignee", "labels", "created", "updated"};
        v.ColumnOrder = {"id"};
        for (const auto& fieldId : v.Fields) {
            v.ColumnOrder.push_back("field:" + fieldId);
        }
        v.ColumnWidths["id"] = 90.0f;
        ws.ActiveViewId = v.Id;
        ws.Views.push_back(std::move(v));
        return ws;
    }

    ViewDefinition defaultView;
    defaultView.Id = "default_view";
    defaultView.Name = "Default View";
    defaultView.Jql = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
    defaultView.Fields = {"summary", "assignee", "priority", "status", "created", "updated"};
    defaultView.ColumnOrder = {"id"};
    for (const auto& fieldId : defaultView.Fields) {
        defaultView.ColumnOrder.push_back("field:" + fieldId);
    }
    defaultView.ColumnWidths["id"] = 90.0f;
    ws.ActiveViewId = defaultView.Id;
    ws.Views.push_back(std::move(defaultView));
    return ws;
}

inline ViewsStore ViewWorkspaceToViewsStore(const ViewWorkspaceState& ws) {
    ViewsStore s;
    s.Version = 2;
    s.ActiveViewId = ws.ActiveViewId;
    s.Views = ws.Views;
    return s;
}

inline void ViewsStoreToViewWorkspace(const ViewsStore& slice, ViewWorkspaceState& ws) {
    ws.ActiveViewId = slice.ActiveViewId;
    ws.Views = slice.Views;
}

} // namespace SmatchetViewsDiskDetail

struct PathRemapRule {
    std::string FromPrefix;
    std::string ToPrefix;
};

/** RGBA for Blame Analysis UI (ImGui); each array is {r,g,b,a} in 0..1. */
struct BlameUiThemeColors {
    float StatusInfo[4] = {0.55f, 0.92f, 0.75f, 1.0f};
    float StatusError[4] = {1.0f, 0.55f, 0.35f, 1.0f};
    float StatusWarning[4] = {1.0f, 0.85f, 0.2f, 1.0f};
    float FindHighlight[4] = {0.25f, 0.35f, 0.55f, 0.55f};
    float TextDisabled[4] = {0.55f, 0.55f, 0.58f, 1.0f};
    float ImportExisting[4] = {0.65f, 0.82f, 1.0f, 1.0f};
    float ClTooltipTitle[4] = {0.35f, 1.0f, 0.45f, 1.0f};
    float SyntaxKeyword[4] = {0.78f, 0.5f, 1.0f, 1.0f};
    float SyntaxString[4] = {0.95f, 0.65f, 0.45f, 1.0f};
    float SyntaxComment[4] = {0.45f, 0.75f, 0.45f, 1.0f};
    float SyntaxNumber[4] = {0.65f, 0.85f, 1.0f, 1.0f};
    float SyntaxPreprocessor[4] = {0.85f, 0.85f, 0.5f, 1.0f};
};

/** Settings for the Blame Analysis tool (stored under `blame_analysis` in smatchet_config.json). */
struct BlameAnalysisConfig {
    std::string P4Executable = "p4";
    std::string P4VcExecutable = "p4vc";
    /** If non-empty, used instead of default `p4vc timelapse -l {line} {file}`. Placeholders: {file}, {line}, {cl}. */
    std::string TimelapseCommandTemplate;
    /** If non-empty, used instead of default `p4vc change {cl}`. Placeholders: {cl}, {file}, {line}. */
    std::string ChangeCommandTemplate;
    /** Opened after "Ask AI" copies context to the clipboard. */
    std::string AiChatUrl;
    int DefaultMaxFrames = 64;
    std::vector<std::string> DefaultIgnoreKeywords;
    std::vector<PathRemapRule> PathRemaps;
    int ChangelistCacheMaxEntries = 512;
    BlameUiThemeColors UiColors{};
    /** Jira field id (e.g. customfield_10001) whose value populates the blame callstack text. */
    std::string CallstackTrackerFieldId;
    /** Jira field id whose value (decimal CL) pre-fills "Before changelist" when blame opens on an issue. */
    std::string LastFoundClTrackerFieldId;
    /** Jira field id (date) pre-filling the "or day" picker when blame opens on an issue; empty if unset or blank. */
    std::string LastOccurrencesTrackerFieldId;
};

class ConfigManager {
  public:
    struct CliOverrides {
        bool HasDbPath;
        std::string DbPath;
        bool HasBackendType;
        std::string BackendType;
        bool HasMcpPort;
        int McpPort;
        bool HasMcpAllowRemote;
        bool McpAllowRemote;

        CliOverrides()
            : HasDbPath(false), DbPath(), HasBackendType(false), BackendType(), HasMcpPort(false), McpPort(0),
              HasMcpAllowRemote(false), McpAllowRemote(false) {}
    };

    // Legacy compatibility entrypoint: use the same base for both runtime assets and writable files.
    static void SetBaseDirectoryForFiles(const std::string& baseDir);
    static void SetRuntimeAssetDirectory(const std::string& baseDir);
    static void SetUserDataDirectory(const std::string& baseDir);

    /** Directory used for writable config/views/cache files (trailing separator if set). Empty if unset. */
    static const std::string& GetFilesBaseDirectory();
    static const std::string& GetRuntimeAssetDirectory();
    static const std::string& GetUserDataDirectory();
    static std::string GetDefaultSettingsPath();

    static nlohmann::json LoadJsonFile(const std::string& path);
    static nlohmann::json LoadMergedConfigJson();
    static std::string NormalizeUiLanguageCode(const std::string& code);
    static void WriteConfigJson(const nlohmann::json& j);

    static void Save(const TrackerConfig& config);
    static BlameAnalysisConfig LoadBlameAnalysis();
    static void SaveBlameAnalysis(const BlameAnalysisConfig& b);

    static TrackerConfig Load(const CliOverrides& cli = CliOverrides());

    static std::string GetConfigPath();
    static std::string GetViewsPath();
    static std::string GetImGuiSettingsPath();

    static const char* GetDefaultImGuiDockLayoutIni();
    static bool WriteDefaultImGuiSettingsFile();
    static void EnsureDefaultImGuiSettingsFile();

    /** Normalize config tracker string to a stable backend bucket key (`Jira` or `Plane`). */
    static std::string NormalizeViewsBackendKey(const std::string& trackerType);

    static PersistentViewsFile LoadPersistentViewsFromDisk();
    static void SavePersistentViewsToDisk(const PersistentViewsFile& disk);
    static void EnsureViewBucketBootstrapped(PersistentViewsFile& disk, const std::string& backendKey,
                                             const TrackerConfig& cfg, bool& outDirty);
    static ViewsStore ViewWorkspaceToViewsStore(const ViewWorkspaceState& ws);
    static void ViewsStoreToViewWorkspace(const ViewsStore& slice, ViewWorkspaceState& ws);

    /** Load+bootstrap active backend slice (used when no in-memory Views wrapper is available). */
    static ViewsStore LoadViewsOrBootstrap(const TrackerConfig& cfg);

    // Crash-safe write: writes to <path>.tmp then atomically renames onto <path>. Used by
    // FieldCatalogCache as well as the internal Write* helpers, so it stays in the public API.
    static bool AtomicWriteTextFile(const std::string& path, const std::string& content);
};

#endif
