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

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "AiTypes.h"
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

    // GitHub-as-tracker configuration (PR2 of docs/design/github-tracker-backend.md).
    // Tracker-role only — independent from the deleted agentic flow's old GitHubPat.
    // PAT is DPAPI-encrypted on Win32 (same code path as AiApiKey); base URL defaults
    // to api.github.com; owner/repo carry the active repository the tracker views.
    std::string GitHubPat;     // Personal Access Token (DPAPI-encrypted on Win32)
    std::string GitHubBaseUrl; // e.g. https://api.github.com or https://<enterprise>/api/v3
    std::string GitHubOwner;   // e.g. "alexandrosk0"
    std::string GitHubRepo;    // e.g. "Smatchet"

    // JQL used when querying Jira; defaults to issues assigned to the current user.
    std::string JqlQuery = "assignee=currentUser()";
    // Jira field keys to extract and cache (e.g. customfield_12345, duedate).
    std::vector<std::string> SelectedFields;
    // When true, show tooltips on hover when grid field text overflows (clipped or multiline).
    // Exposed in UI as Settings -> Preferences -> Appearance.
    bool EnableFieldOverflowTooltips = true;
    // When true (default), single click on a grid cell starts editing. False requires double-click.
    // Exposed in Settings -> Preferences -> Appearance.
    bool SingleClickToEditGridCells = true;
    // When true, tracker-changing actions are disabled. Defaults on only for first launch with no setup config.
    bool ReadOnlyMode = false;
    // True after at least one AuthenticatedReachable probe / live request has been observed.
    // Until then, the main UI is locked to Preferences -> Tracker so the user can finish setup.
    bool BackendHasBeenReachable = false;
    // Wheel ticks at top/bottom before vertical wheel reroutes to horizontal grid scroll.
    // Exposed in UI as Settings -> Preferences -> Appearance.
    int GridEndWheelSwallowsBeforeHorizontal = 15;
    // Main window state restored at launch. X/Y == -1 means "unset; use OS default centring".
    int WindowX = -1;
    int WindowY = -1;
    int WindowWidth = 1280;
    int WindowHeight = 720;
    bool WindowMaximized = false;
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
    // When true, tracker backends log truncated HTTP response bodies at Trace.
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
    // GitHub field ids copied from the last grid row when seeding a new-issue draft (+ New issue).
    std::vector<std::string> NewIssueInheritFieldIdsGitHub;
    // One-shot migration flag: injects "issuetype" into both inherit lists for users who saved
    // their config before issuetype was added to the defaults. Persisted so the injection only
    // fires once — the user is free to remove "issuetype" again afterwards.
    bool MigratedInheritIssueTypeV1 = false;

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
    bool ShowPrimarySideBar = true;
    bool ShowSecondarySideBar = false;
    bool ShowPanel = true;
    bool ShowStatusBar = true;

    // UI density: controls ItemSpacing / FramePadding applied each frame.
    enum class UiDensity : int { Compact = 0, Normal = 1, Comfortable = 2 };
    UiDensity Density = UiDensity::Normal;

    // Panel dock position: Bottom (default) or docked to the Right side.
    enum class PanelPosition : int { Bottom = 0, Right = 1 };
    PanelPosition PanelDockSide = PanelPosition::Bottom;

    // Side bar orientation: true = right (VS Code default), false = left.
    bool PrimarySideBarOnRight = true;

    // --- Smatchet Assistant (AI) — Phase A' ---
    // AiProvider enum value persisted as int; unknown / out-of-range values clamp to 0 (OpenAi)
    // on Load so a future-version config opened by an older build degrades safely.
    int AiProviderKind = 0;
    // OpenAI (or OpenAI-compatible) API key. Persisted DPAPI-encrypted on Win32; legacy plaintext
    // `ai_api_key` keys from earlier exploratory builds are migrated on Load and erased on next Save.
    std::string AiApiKey;
    // Anthropic API key. Same DPAPI + legacy-migration shape as `AiApiKey`.
    std::string AiAnthropicApiKey;
    // DeepSeek API key. DeepSeek's wire is OpenAI-protocol-compatible
    // (`/v1/chat/completions`); the dedicated key field keeps the slot
    // round-tripping across provider switches so the user does not lose
    // credentials by flipping to / from another provider. Persisted
    // DPAPI-encrypted on Win32; legacy plaintext `ai_deepseek_api_key`
    // keys from any hand-edited config are migrated on Load and erased
    // on next Save. Same shape as `AiApiKey` / `AiAnthropicApiKey`.
    std::string AiDeepSeekApiKey;
    // Ollama native endpoint (e.g. http://localhost:11434). Stored verbatim — Phase D consumer.
    std::string AiOllamaBaseUrl;
    // Generic base URL for `OpenAi` / `OllamaOpenAiCompat`; empty means provider default
    // (`https://api.openai.com`). Stored verbatim — empty round-trips as empty.
    std::string AiBaseUrl;
    std::string AiModelOpenAi = "gpt-4o-mini";
    std::string AiModelAnthropic = "claude-sonnet-4-6";
    std::string AiModelOllama = "llama3";
    // DeepSeek base URL (empty = use built-in default `https://api.deepseek.com`).
    // Stored verbatim — same shape as `AiBaseUrl` for OpenAI. Kept as a separate
    // field so the URL round-trips across provider switches.
    std::string AiDeepSeekBaseUrl;
    // DeepSeek model identifier. Default `deepseek-chat`; the published catalog
    // (see AiModelCatalog) also lists `deepseek-reasoner` (reasoning-tuned).
    std::string AiModelDeepSeek = "deepseek-chat";
    /// Reasoning effort (OpenAI `reasoning_effort` parameter on o-series / reasoning-tuned
    /// models; LM Studio passes it through to local reasoning models such as Qwen3 / gemma-3).
    /// Allowed values: "auto" | "low" | "medium" | "high". "auto" means "do not send the
    /// parameter — let the server decide". The default-applies-to-all-providers semantics
    /// match the existing model field; providers that don't understand the param ignore it.
    std::string AiReasoningEffort = "auto";
    bool AssistantPanelOpen = false;
    // Historical floating-window width; deprecated since the panel was dock-integrated.
    // The field is still serialized (additive default load + round-trip) so v6 configs
    // can downgrade to an older build without losing other fields, but the dock node
    // now owns sizing — this value is no longer consulted at runtime.
    float AssistantPanelWidth = 380.0f;
    // When true, the assistant panel docks to the right secondary side bar instead of
    // the default left primary side bar. Toggled via the swap-side button in the
    // panel header.
    bool AssistantPanelOnSecondarySide = false;
    // Global agents.md path. Default-at-Load (not default-at-construct): when blank on Load
    // and a platform shared user-data dir is available, ConfigManager fills this with
    // `<shared>/agents.md`. Construct-time default would have made the empty/blank distinction
    // impossible — users who explicitly cleared the field would silently get the default back.
    std::string AgentsMdGlobalPath;
    std::string ProjectAgentsMdPath;
    // When true (and the explicit ProjectAgentsMdPath is empty), AgentsMdLoader walks up
    // from the process cwd looking for `agents.md` / `AGENTS.md`. Default off: users
    // running Smatchet from inside an unrelated repo (e.g. the Smatchet source tree
    // itself) otherwise pick up that project's `AGENTS.md` as their assistant prompt.
    bool AgentsMdAutoDiscoverProject = false;
    bool AssistantContextBlockSelection = true;
    bool AssistantContextBlockVisibleRows = true;
    bool AssistantContextBlockActiveTicket = true;
    bool AssistantContextBlockActiveView = true;
    // Default OFF: audit-trail blocks ship assignee emails, freeform comments, and custom-field
    // values via BackendAuditTrail::ReadRecentEvents to the configured AI provider. New users
    // opt-in explicitly via the Preferences > Assistant > Context tab. First-send consent modal
    // (per docs/backlog/agent-self-improvement/security.md 2026-05-17 P1) tracked separately.
    bool AssistantContextBlockAuditTrail = false;
    /// Hard cap on persisted chat-history rows (excluding pinned messages, which are
    /// exempt from trim). Drives `LocalCacheManager::TrimChatMessages` after every
    /// successful append + caps the initial hydration window on first frame. Default
    /// 500 keeps the SQLite file small while preserving multi-day conversation depth.
    /// Per AGENTS.md "hold the schema-version bump until verified end-to-end" — the
    /// shipped config-schema bump for this feature happens when Phase 7 lands, not on
    /// every intermediate edit. (Phase 3 of ai-chat-claude-desktop-parity.)
    int AssistantHistoryMaxRows = 500;
    // When true, the Preferences "Save changes" button runs a live ProbeReachability
    // probe before committing the Assistant tab buffers. Static validation
    // (`AiPrefsValidator`) must still pass first; the probe is the live network /
    // auth check on top. Default on — surfaces credential mistakes at Save time
    // rather than at first-message time. Uncheck to commit offline-entered
    // credentials without contacting the provider.
    bool AiPrefsVerifyOnSave = true;

#if defined(SMATCHET_WITH_WHISPER)
    // --- Whisper dictation (push-to-talk) — Phase A schema (additive; no schema bump). ---
    // See docs/design/whisper-dictation.md § Config schema additions.
    //
    // Runtime opt-in gate. Even when SMATCHET_WITH_WHISPER=ON the plugin stays
    // dormant (no mic access, no network, no model download) until this flips to
    // true via the first-run setup dialog (Phase C) or Preferences (Phase F).
    bool WhisperEnabled = false;
    // True once the user has seen and answered the first-run setup dialog (Phase C).
    // Set independently of WhisperEnabled — a user who chose "No thanks" has
    // SetupCompleted=true + Enabled=false; the dialog never reappears.
    bool WhisperSetupCompleted = false;
    // Informational record of the user's setup choice: "enabled", "disabled", or
    // "" (deferred — show dialog next launch). The active runtime gate is
    // WhisperEnabled; this field exists for UX readouts (Preferences) and audit.
    std::string WhisperSetupChoice;
    // Backend selection: "auto" (default — local if present, cloud fallback),
    // "local" (whisper.cpp only), or "cloud" (OpenAI Whisper API only).
    std::string WhisperMode = "auto";
    // Local model name (Phase C uses this to resolve <userData>/whisper/<name>.bin).
    std::string WhisperModel = "ggml-base.en";
    // Push-to-talk hotkey. Captured via the Preferences capture widget in Phase E.
    std::string WhisperHotkey = "Ctrl+Alt+Space";
    // OpenAI API key for /v1/audio/transcriptions. Persisted DPAPI-encrypted on
    // Win32; same legacy-plaintext migration shape as AiApiKey. Phase B consumer.
    // Falls back to AiApiKey only when this is empty AND AiProvider==OpenAi
    // (see docs/design/whisper-dictation.md § API key fallback rule).
    std::string WhisperApiKey;
    // Unix epoch seconds stamped when the user actively clicked "Download +
    // enable" in the setup banner or the "Download" button in Preferences.
    // WhisperConsentGate::CanDownloadModel rejects any download whose stamp is
    // older than the freshness window (30 s by default). Enforces consent
    // invariant #2 ("no silent re-downloads, no resume-after-restart without
    // re-confirming") — see docs/design/whisper-dictation.md § Consent
    // invariants. Persisted so a future Preferences "re-run setup" flow can
    // observe whether the user has ever consented.
    std::int64_t WhisperConsentTimestampSec = 0;
    // Phase F — language hint forwarded to whisper.cpp (`whisper_full_params.language`)
    // and to OpenAI's multipart `language` field. `"en"` selects English (default — the
    // ggml-*.en models are English-only anyway); `"auto"` (or empty) asks the backend
    // to autodetect from the 80+ supported language codes.
    std::string WhisperLanguage = "en";
    // Phase F — strip leading/trailing silence from captured PCM before insertion / upload.
    // Implementation in Plugins/Whisper/SilenceTrim.cpp (peak-relative amplitude gate over
    // 100 ms windows). Off-by-default for users who prefer raw audio; on by default for new
    // installs because it tightens both latency (smaller cloud payload) and noise.
    bool WhisperTrim = true;
    // Phase F — hard cap on captured-clip length in seconds. 0 disables the cap; positive
    // values clamp the captured PCM (post-trim) to `WhisperMaxClipSec` seconds before the
    // worker dispatches transcription. Cap is clamped at <= 600 s (10 min) to guard
    // against runaway cloud cost (~$0.006/minute on OpenAI Whisper API).
    int WhisperMaxClipSec = 60;
    // Phase F — when true, a transcription that lands on the AI Assistant chat input AND
    // ends with sentence-final punctuation (".", "!", "?") triggers the same Send action
    // a user clicking the Send button would. Off-by-default; opt-in because hands-free
    // chat is a power-user shape and accidental sends are expensive.
    bool WhisperAutoSendOnPunctuation = false;
#endif

    // --- Transient UI state — not round-tripped through JSON. Reset on every launch. ---
    bool FullScreen = false;
    bool ZenMode = false;

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
    // Fresh installs land on the bright ImGui-built-in dark palette. Existing users keep
    // whatever theme they previously persisted — ConfigManager::Load overwrites this default
    // with the string from disk, so a config with `"theme": "smatchet_dark"` round-trips
    // through to ThemeId::SmatchetDark unchanged.
    ThemeId Theme = ThemeId::ImGuiDefaultDark;
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

/** RGBA for Blame Analysis UI status / find / tooltip widgets (ImGui); each array is {r,g,b,a} in 0..1.
 *  C++ syntax colors live on the active theme — see SmatchetTheme::GetSyntaxColors(). */
struct BlameUiThemeColors {
    float StatusInfo[4] = {0.55f, 0.92f, 0.75f, 1.0f};
    float StatusError[4] = {1.0f, 0.55f, 0.35f, 1.0f};
    float StatusWarning[4] = {1.0f, 0.85f, 0.2f, 1.0f};
    float FindHighlight[4] = {0.25f, 0.35f, 0.55f, 0.55f};
    float TextDisabled[4] = {0.55f, 0.55f, 0.58f, 1.0f};
    float ImportExisting[4] = {0.65f, 0.82f, 1.0f, 1.0f};
    float ClTooltipTitle[4] = {0.35f, 1.0f, 0.45f, 1.0f};
};

/** Settings for the Blame Analysis tool (stored under `blame_analysis` in smatchet_config.json). */
struct BlameAnalysisConfig {
    /**
     * Runner seam for `p4` invocations (slice 3 of autonomous-debugging-no-creds).
     * Tests install a `tests/support/FakeP4Runner.h` lambda to drive
     * `P4Blame.cpp:P4RunCommand` end-to-end without spawning the real binary.
     * Empty (default) → real `SubprocessCapture::Run` path; production behaviour
     * preserved.
     */
    using P4RunCommandFn = std::function<bool(const std::vector<std::string>& args, int& outExitCode,
                                              std::string& outStdout, std::string& outStderr)>;
    P4RunCommandFn P4RunOverride;
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
    // Bumped to 2: schema-1 imgui.ini was corrupted by the old prepareTopLevelWindow bug
    // (SetNextWindowPos kicked panels out of dock nodes). Forces a one-shot reset so the
    // fixed layout + SetNextWindowDockID fallbacks pick up correctly.
    // Bumped to 3: rewrote default dock ini to a clean VS-shell tree (top split with
    // central + primary side bar + reserved secondary, bottom panel hosting Log/Audit/etc.).
    // Schema 2 is the milestone version. Boot path: Load cfg, compare schema,
    // if cfg.LayoutSchemaVersion < this, WriteDefault + persist, then ImGui auto-loads
    // the fresh ini via io.IniFilename. Pre-first-frame migration (matters because
    // runtime LoadIniSettingsFromDisk does NOT re-parent already-created windows).
    // Schema 3: Performance window DockId changed from 0x7 (invalid Split=X parent node)
    // to 0xA,7 (bottom-panel tab). Node 0x7 is a container, not a leaf — ImGui floats
    // any window docked into a parent node.
    // Bumped to 4: Annotate is now an embedded tab inside Smatchet - Active Project (no
    // longer a separate docked window). Removed dock nodes 0x7 (intermediate X-split) and
    // 0x3 (former 250px right pane for Annotate/Source Blame). Node 0x2 (Active Project)
    // is now a direct child of 0x1 alongside 0x8 (Views), giving the grid more width.
    // Bumped to 5: added NoTabBar=1 to central node 0x2 so the dock-node tab header
    // ("Smatchet - Active Project") is suppressed — the in-window Grid / Annotate
    // tab bar is the only chrome the user sees on the active project panel.
    // Bumped to 6: AI assistant side-panel feature shipped (Phases A-E of
    // docs/design/ai-assistant-side-panel.md). One-shot reset surfaces the new
    // right-anchored AI panel in the default VS-shell layout; the previous v5
    // ini stays valid (no panel in the dock tree means no migration error),
    // the bump just guarantees the new dock ID picks up cleanly on first launch
    // after the feature lands. All Ai* TrackerConfig fields default-load via
    // `j.value(..., default)` so v4 / v5 configs continue to load unchanged.
    static const int kCurrentLayoutSchemaVersion = 7;

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

    /** Where writable files (config / views / SQLite cache / imgui.ini) live.
     *   - `Portable`  → next to the runtime assets (exe dir for standalone,
     *                   `<UnrealProject>/Saved/` for the plugin).
     *   - `Shared`    → OS user-data dir (`%LOCALAPPDATA%\Smatchet` on Windows,
     *                   `~/Library/Application Support/Smatchet` on macOS,
     *                   `$XDG_CONFIG_HOME/Smatchet` on Linux). */
    enum class StoragePreference { Portable, Shared };

    /** Persisted marker file storing the user's explicit choice. Lives alongside the
     *  runtime assets so it survives reinstalls of the OS-level user data. Absent file
     *  means "no explicit choice — caller's default applies" — standalone defaults to
     *  `Shared`, Unreal plugin defaults to `Portable` (preserves legacy per-project storage). */
    static std::string GetStoragePreferenceFlagPath(const std::string& runtimeAssetDir);

    /** Read explicit user preference from the marker file. When absent or malformed,
     *  return `defaultIfMissing`. Cheap (one stat + one short read). */
    static StoragePreference GetStoragePreference(const std::string& runtimeAssetDir,
                                                  StoragePreference defaultIfMissing);

    /** Write the explicit preference. Takes effect on next launch — paths are resolved
     *  at startup. Returns true on success; on failure `outError` is populated. */
    static bool SetStoragePreference(const std::string& runtimeAssetDir, StoragePreference pref, std::string& outError);

    /** Returns true iff the user has made an explicit choice (marker file exists). */
    static bool HasExplicitStoragePreference(const std::string& runtimeAssetDir);

    /** Platform-resolved shared user-data directory (trailing separator). Empty when the
     *  OS doesn't expose the expected env vars (degraded — caller should fall back to
     *  the runtime asset dir). Cross-platform: %LOCALAPPDATA% \ macOS Library \ XDG. */
    static std::string GetPlatformSharedUserDataDirectory();

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
