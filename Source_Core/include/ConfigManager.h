#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

// Public surface of the Smatchet config persistence layer. The implementation lives in
// Source_Core/src/ConfigManager.cpp.
//
// Build-time win: this header pulls only <nlohmann/json_fwd.hpp> (a ~75 LOC forward-decl
// header) instead of the full <nlohmann/json.hpp> (~30 k LOC of templated code). Every TU
// that needs a TrackerConfig field used to pay the json.hpp parse cost; now only the few
// TUs that actually construct/parse json values (this .cpp, plus call sites that compose
// `nlohmann::json` directly) include the full header.
//
// Friend serializers (CommentTemplate::to_json / from_json) are declared here and defined
// in the .cpp — nlohmann's adl_serializer finds them via ADL from any TU that includes
// this header and uses j["…"] = config.QuickCommentTemplates.

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "SmatchetDefaults.h"
#include "SmatchetThemeIds.h"

struct CommentTemplate {
    std::string Id;
    std::string Title;
    std::string Text;

    // Bodies in ConfigManager.cpp — declaration is sufficient for ADL lookup at call sites.
    friend void to_json(nlohmann::json& j, const CommentTemplate& t);
    friend void from_json(const nlohmann::json& j, CommentTemplate& t);
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

    // Tracker Type: "Jira" or "Plane"
    std::string TrackerType = SmatchetDefaults::kDefaultBackendType;

    // Plane.so specific configuration
    std::string PlaneUrl;           // API origin: https://api.plane.so (no path); https://app.plane.so normalized
    std::string PlaneWorkspaceSlug; // e.g. "my-workspace"
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
    // PR 3: max distinct projects retained in the on-disk field catalog cache; LRU evicted past cap.
    // 0/negative falls back to the in-cache default (16).
    int FieldCatalogCacheMaxProjects = 16;
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
    // Views editor sidebar (saved-view list) width, modern two-pane layout.
    float ViewsSidebarWidth = 220.0f;
    // Views editor: split between Available (left) and Selected (right) panes inside the Fields tab.
    float ViewsFieldsSplitRatio = 0.5f;

    // VS Code shell: dockspace node visibility (View > Appearance toggles + shortcuts).
    bool ShowPrimarySideBar   = true;
    bool ShowSecondarySideBar = false;
    bool ShowPanel            = true;
    bool ShowStatusBar        = true;

    // UI density: controls ItemSpacing / FramePadding applied each frame.
    enum class UiDensity : int { Compact = 0, Normal = 1, Comfortable = 2 };
    UiDensity Density = UiDensity::Normal;

    // Panel dock position: Bottom (default) or docked to the Right side.
    enum class PanelPosition : int { Bottom = 0, Right = 1 };
    PanelPosition PanelDockSide = PanelPosition::Bottom;

    // Side bar orientation: true = right (VS Code default), false = left.
    bool PrimarySideBarOnRight = true;

    // Transient UI state — not persisted to disk.
    bool FullScreen = false; ///< transient, not serialized
    bool ZenMode    = false; ///< transient, not serialized

    // Bumped to kCurrentLayoutSchemaVersion after the first VS-shell layout migration.
    // On first launch with an old imgui.ini the migration resets the dock layout, then
    // writes this field so subsequent launches skip the reset.
    int LayoutSchemaVersion = 0;

    // Font setting
    std::string SelectedFontName = "Segoe UI";
    // Font size in points, used by View > Appearance > Zoom In/Out/Reset.
    // Clamped to [8, 32] at load. 16 matches the legacy hardcoded value.
    int FontSizePt = 16;
    // Active ImGui style palette, applied per-frame from View > Appearance > Theme.
    // Default keeps the legacy palette bit-identical for existing users.
    ThemeId Theme = ThemeId::SmatchetDark;
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

// Note: the previous public `SmatchetViewsDiskDetail::*` namespace (Parse / Serialize / default
// view helpers) was moved into the anonymous namespace of ConfigManager.cpp. They had exactly one
// caller (ConfigManager.cpp itself); leaving them inline in the header forced every consumer to
// re-parse ~140 LOC of nlohmann::json template-using code for no benefit.

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
    // Bump when the default dock layout changes incompatibly. SmatchetUI::Draw
    // detects LayoutSchemaVersion < kCurrentLayoutSchemaVersion on first launch
    // after upgrade, resets imgui.ini, then persists the new version so the
    // migration runs exactly once.
    static const int kCurrentLayoutSchemaVersion = 1;

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

    /// Invalidate the in-process Load() cache so the next call re-reads from disk.
    /// Call after WriteConfigJson() to ensure the change is visible without restarting.
    static void InvalidateCache();

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
