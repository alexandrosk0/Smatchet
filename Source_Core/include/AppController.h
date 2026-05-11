#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

// 1. MUST BE INCLUDED FIRST FOR GCC 13+ COMPATIBILITY
#include <limits>
#include <cstdint>

// 2. Lua / sol2 (optional build — see SMATCHET_WITH_LUA_AUTOMATION in CMake)
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#endif

// 3. THE REST OF YOUR INCLUDES
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include "LocalCacheManager.h"
#include "ITrackerClient.h"
#include "MainThreadDispatcher.h"
#include "IssueDraft.h"
#include "IssueCreatePipeline.h"
#include "JiraClient.h"

#include <nlohmann/json.hpp>

class PluginHost;

/** Single consolidated tracker degraded/offline banner for main windows (replaces stacked warnings). */
struct TrackerConnectivityBannerForUi {
    enum class Level { None, Warning, Error };
    Level Kind = Level::None;
    std::string Message;
};

/** Raw tracker issue fetch result; apply on the UI thread via AppController::ApplyIssueFetchPack. */
struct TrackerIssueFetchPack {
    std::vector<CachedTicket> Tickets;
    bool FullSyncCompleted = false;
    std::string FetchError;
    /// Soft caveat (e.g. pagination cap). See TrackerIssueFetchSummary::Warning.
    std::string Warning;
};

struct AppUpdateAsset {
    std::string Name;
    std::string DownloadUrl;
};

struct AppUpdateInfo {
    bool Ok = false;
    bool UpdateAvailable = false;
    std::string CurrentVersion;
    std::string LatestVersion;
    std::string ReleaseTag;
    std::string ReleaseUrl;
    std::string ReleaseNotes;
    std::string Error;
    AppUpdateAsset InstallerAsset;
};

class ITrackerBackendFactory;
class OfflineQueueService;

class AppController {
    /// `OfflineQueueService` needs access to AppController-private state (`Cache`, `Backend`,
    /// `AvailableFields`, the offline-replay mutex, etc.) during the extraction transition.
    /// See CODE_REVIEW.md §1.7 / §7 item 12 — Phase 2 will replace this with a small set of
    /// interface bundles so the access is no longer trusted-friendship-based.
    friend class OfflineQueueService;

  public:
    ~AppController();

    struct FieldEditResult {
        bool Ok = false;
        std::string Error;
        std::unordered_map<std::string, std::string> UpdatedDisplayValues;
    };

    /// Inject a custom tracker-backend factory. Must be called BEFORE `Initialize` to take
    /// effect on the first backend instantiation. Tests / Unreal-host embeddings use this
    /// to substitute a mock or alternative-transport client; the default standalone build
    /// leaves it null and `Initialize` lazily wires `DefaultTrackerBackendFactory`.
    void SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory);

    void Initialize(const std::string& dbPath, const std::string& backendType);

    /** Path passed to `Initialize` (may be relative to the process working directory). */
    const std::string& GetLocalCacheDbPath() const { return localCacheDbPath_; }
    /** Absolute path for display; falls back to the raw path if resolution fails. */
    std::string GetResolvedLocalCacheDbPath() const;
    /**
     * UI thread: closes SQLite, deletes the cache file (and WAL sidecars), opens a new empty database,
     * clears in-memory tickets, and resets streaming sync state. On success, call `SyncWithBackend`
     * to refill from the tracker.
     */
    bool RecreateLocalCacheDatabase(std::string& outError);

    /// Worker-to-UI-thread deferred task queue (CODE_REVIEW.md §6.1). Post lambdas here from any
    /// thread; SmatchetUI::Draw drains them at the top of each frame. Use instead of ad-hoc atomics.
    MainThreadDispatcher mainThreadDispatcher;

    /** Call from plugins in OnEarlyInit only (before Initialize completes InitLua). */
    void AddAutomationLogSink(std::function<void(const std::string&)> sink);
    /** Drop all sinks. Call before destroying plugins to avoid dangling `[this]` captures. */
    void ClearAutomationLogSinks();

    /**
     * Optional host callback for launching URLs.
     * Use this when embedding in Unreal (avoids OS-level `system("start")` calls).
     */
    void SetOpenUrlHandler(std::function<void(const std::string&)> handler);

    /** Opens a URL using the handler if set; otherwise falls back to default browser. */
    void OpenUrl(const std::string& url) const;

    /**
     * Optional host callback to hide the embedded overlay (e.g. Unreal plugin toggles Slate visibility).
     * Set by SmatchetImGuiHost; no-op when unset (standalone / tests).
     */
    void SetCloseEmbeddedUiHandler(std::function<void()> handler);
    void CloseEmbeddedUi();
    void SetRequestAppQuitHandler(std::function<void()> handler);
    void RequestAppQuit() const;

    /** Standalone / embedded host: set so Preferences can start or stop MCP without app restart. */
    void SetRuntimePluginHost(PluginHost* host);
    PluginHost* RuntimePluginHost() const { return runtimePluginHost_; }

#if defined(SMATCHET_WITH_MCP)
    /** Bounded ring buffer of MCP-related actions (thread-safe). */
    void AppendMcpActivity(const std::string& line);
    std::vector<std::string> CopyMcpActivityLog() const;
    /** MCP HTTP server: any routed request after auth gate (worker threads). */
    void NotifyMcpClientHttpActivity();
    /** @return false if no client request has been recorded yet this process. */
    bool TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const;
#endif

    /**
     * Optional host callback for showing tracker attachments inside Unreal.
     * If set, Smatchet will download the attachment bytes and save them to a local temp file,
     * then call this handler with the file path.
     */
    using AttachmentViewerHandler =
        std::function<void(const std::string& localPath, const std::string& mimeType, const std::string& filename)>;
    void SetAttachmentViewerHandler(AttachmentViewerHandler handler);
    using AttachmentPreviewHandler = std::function<bool(const std::string& localPath, const std::string& mimeType,
                                                        const std::string& filename, const std::string& sourceUrl)>;
    void SetAttachmentPreviewHandler(AttachmentPreviewHandler handler);
    struct AttachmentDescriptor {
        std::string Filename;
        std::string Url;
        std::string MimeType;
    };
    using AttachmentCollectionHandler = std::function<void(const std::vector<AttachmentDescriptor>& attachments)>;
    void SetAttachmentCollectionHandler(AttachmentCollectionHandler handler);
    void ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments);

    /**
     * Optional host hook for native multi-file open (new-issue attachments, etc.).
     * Invoked on the UI thread; implementation may block (e.g. Win32 IFileDialog).
     * If unset, RequestOpenFilePaths completes with an empty vector.
     */
    using OpenFilePathsHandler =
        std::function<void(bool allowMultiple, const std::string& initialDirectoryUtf8,
                           std::function<void(std::vector<std::string> absolutePathsUtf8)> onComplete)>;
    void SetOpenFilePathsHandler(OpenFilePathsHandler handler);
    void RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,
                              std::function<void(std::vector<std::string>)> onComplete) const;

    /**
     * Open an attachment (image/pdf/etc) without requiring Basic Auth headers in the browser.
     * - If AttachmentViewerHandler is set: downloads to a temp file and calls the handler.
     * - Otherwise, for image mime types: downloads and offers in-app preview handler.
     * - If no host/in-app handler path is available: falls back to OpenUrl(url).
     */
    void OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType);
    /** Download to temp then open local file in OS default app (matches Unreal attachment viewer). */
    void OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename, const std::string& mimeType);
    bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError = nullptr);
    std::string GetAppVersion() const;
    std::string GetGitHubReleaseRepo() const;
    AppUpdateInfo CheckForAppUpdate(bool includePrerelease = false) const;
    bool DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                          std::string& outError) const;

    void InitLua();
    void InitLuaCore(sol::state& state);
    void InitLuaUi(sol::state& state);

    std::string ResolveLuaScriptPath(const std::string& filename) const;
    /** Basenames of `*.lua` files in the configured scripts directory (non-recursive). */
    std::vector<std::string> ListLuaScriptFiles() const;

    /**
     * Resolve a URL or local path for field icons / Lua `imgui.image`.
     * Allows http(s) URLs; local files must lie under the Lua scripts directory or the runtime asset directory.
     */
    std::string ResolveFieldIconAssetPath(const std::string& pathOrUrl) const;

    void RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds);
    void RunFlatScriptAsync(const std::string& scriptPath);

    std::string GetAutomationScriptContent();
    bool SaveAutomationScriptContent(const std::string& content, std::string& outError);

    /** Run a Lua file once (e.g. SmatchetHooks.lua) to register UI hooks; errors go to automation log sinks. */
    void RunLuaSetupScript(const std::string& scriptPath);

    /** Present with or without Lua build; no-op / empty when `SMATCHET_WITH_LUA_AUTOMATION` is off. */
    std::vector<std::string> GetLuaTicketActionNames() const;
    void ExecuteLuaTicketAction(const std::string& name, const std::string& issueId);
    std::vector<std::string> GetLuaGlobalActionNames() const;
    void ExecuteLuaGlobalAction(const std::string& name);
    /**
     * Run a one-off Lua chunk from the automation UI (same globals as hooks: smatchet, ui, tracker, …).
     * On failure sets @p outError; on success clears @p outError and may set @p outResultSummary from the
     * first return value (short string / JSON, truncated when long).
     */
    bool ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError, std::string& outResultSummary);
    /** Lua `register_field_icon_map`; returns false when Lua automation is disabled. */
    bool TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field, const std::string& rawValue,
                                  std::string& outPathOrUrl) const;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    /**
     * Lua InitLua uses one functor struct per binding (see AppController_LuaBindings.cpp) so sol2/GCC never
     * merges unrelated lambdas or member-wrappers that share the same demangled metatable name.
     */
    void LuaLogInfoBind(const std::string& msg);
    std::tuple<sol::object, std::string> LuaGetTicketBind(const std::string& issueId);
    std::tuple<sol::object, std::string> LuaDecodeJsonBind(const std::string& s);
    void LuaRegisterFieldDisplayBind(const std::string& fieldId, sol::function fn);
    void LuaUnregisterFieldDisplayBind(const std::string& fieldId);
    void LuaRegisterFieldDisplayByNameBind(const std::string& displayName, sol::function fn);
    void LuaUnregisterFieldDisplayByNameBind(const std::string& displayName);
    void LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map, sol::optional<bool> byName);
    void LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName);
    void LuaImGuiTextBind(const std::string& s);
    void LuaImGuiTextUnformattedBind(const std::string& s);
    bool LuaImGuiImageBind(const std::string& path, float w, float h);
    void LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn);
    void LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback);
    std::vector<CachedTicket> LuaGetActiveTicketsBind();
    /** Live create or offline queue from a Lua spec table; see LUA_GUIDE.md. */
    std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::table spec);
    void ClearLuaTicketContextGlue();

    struct McpToolDefinition {
        std::string name;
        std::string description;
        nlohmann::json parametersSchema;
        sol::protected_function callback;
    };
    /** Thread-safe snapshot (e.g. MCP server thread vs Lua registration on the app thread). */
    std::vector<McpToolDefinition> GetLuaMcpTools() const;
    std::string ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson, std::string& outError);
    std::string ExecuteLuaSnippetForMcp(const std::string& code, const nlohmann::json& args, std::string& outError);
    std::string ExecuteLuaScriptForMcp(const std::string& scriptName, const nlohmann::json& args, std::string& outError);
    void DrawLuaWindows();
#endif

    /**
     * If a Lua handler was registered for fieldId, invoke it to draw the grid cell.
     * @return true if the handler ran and returned a Lua-truthy value (cell fully handled).
     */
    bool TryLuaFieldDisplay(const std::string& fieldId, const CachedTicket& ticket, const std::string& rawValue,
                            float availWidth, const TrackerField* fieldMeta);

    /**
     * Sync issues from the tracker into the local cache.
     * Pass the in-memory UI config + views store when syncing from the app so JQL/fields match
     * the active view without relying on an immediate disk round-trip.
     */
    void SyncWithBackend(const TrackerConfig* configOverride = nullptr, const ViewsStore* viewsOverride = nullptr);

    /** Process queued streaming ticket batches on the UI thread (frame-budgeted). */
    void TickStreamingApply();

    /** Clears the live ticket-sync warning banner (e.g. before kicking off a background fetch). */
    void ClearLastTrackerTicketSyncWarning();

    /**
     * Runs FetchIssues under an internal mutex (safe with concurrent UI-triggered syncs).
     * Does not touch the SQLite cache; pair with ApplyIssueFetchPack on the UI thread.
     */
    TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr);

    /** Merges fetch results into the local cache and updates connectivity banners. */
    void ApplyIssueFetchPack(TrackerIssueFetchPack pack);

    void RefreshLocalData();
    /** Reload ActiveTickets from cache and kick per-issue-type editmeta warmup (same tail as SyncWithBackend). */
    void RefreshLocalDataAndWarmIssueTypeMeta();

    void UpdateTicket(const CachedTicket& ticket);

    std::vector<CachedTicket> GetActiveTickets() const;
    /** Cheap read: shared_ptr to last published ticket list (thread-safe with MCP / workers). */
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const;
    std::uint64_t GetActiveTicketsRevision() const { return ActiveTicketsRevision.load(); }
    bool IsStreamingSyncActive() const {
        return activeStreamingSync_.Active.load() || activeStreamingSync_.ActiveSessionRunning.load() ||
               isDeletingStale_.load();
    }

    /** Bumped when the field catalog changes (fetch, error clear, etc.); UI sort cache should invalidate. */
    std::uint64_t GetFieldCatalogRevision() const { return TrackerFieldCatalogRevision.load(); }

    bool RefreshFieldCatalog(const TrackerConfig& cfg);
    bool FetchFieldCatalog(const TrackerConfig& cfg, TrackerFieldCatalogResult& outCatalog, std::string& outError) const;

    /** Resolve a raw tracker value (e.g. accountId, label UUID) to a display name. */
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const;

    std::string BuildIssueBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const;
    static std::string BuildJqlSearchUrl(const TrackerConfig& cfg, const std::string& jql);

    const std::vector<TrackerField>& GetAvailableFields() const { return AvailableFields; }
    const std::vector<TrackerComponent>& GetAvailableComponents() const { return AvailableComponents; }
    const std::string& GetFieldCatalogError() const { return LastTrackerFieldCatalogError; }
    const std::string& GetFieldCatalogWarning() const { return LastTrackerFieldCatalogWarning; }
    /** Set when a live JQL refresh failed with a transport-style error; UI may show cached tickets. */
    const std::string& GetLastTicketSyncWarning() const { return LastTrackerTicketSyncWarning; }

    /**
     * One banner for field-catalog error/warning, ticket-list cache warning, and optional session note
     * (e.g. Views dashboard users-fetch warning). Prefer this over separate `GetFieldCatalogWarning` /
     * `GetLastTicketSyncWarning` lines in headers.
     */
    TrackerConnectivityBannerForUi GetTrackerConnectivityBannerForUi(const std::string* sessionCatalogNote = nullptr) const;

    /** Last outcome of periodic tracker probe (UI thread). */
    enum class TrackerConnectivityState {
        Unknown,
        AuthenticatedReachable,
        ReachableAuthOrConfigError,
        TransportDown,
        ServiceUnavailable,
    };
    /** Rate-limited background probe; updates connectivity state and recovery latch. */
    void TickTrackerConnectivityMonitor(const TrackerConfig& cfg);
    /** Latest reachability from background probe (or after a successful live backend request). */
    TrackerConnectivityState GetLastTrackerConnectivityState() const { return lastTrackerConnectivityState_; }
    /**
     * One-shot: true when reachability improved to authenticated-reachable (including from
     * transport-down, service-unavailable, or auth/config errors, and cold-start when a catalog
     * offline banner is still set). Clears ticket sync + field-catalog warnings and nudges
     * offline replay timers. UI should run catalog refetch + `SyncWithCurrentView` on the same frame.
     */
    bool ConsumeTrackerConnectivityRecovery();
    /**
     * One-shot: true after a successful live `SyncWithBackend` issue fetch cleared a stale offline
     * field-catalog banner. UI should set `triggerCatalogRefetch` (same as connectivity recovery).
     */
    bool ConsumeFieldCatalogRefetchAfterLiveTicketSync();
    /**
     * Main-thread: Dispatches any deferred UI notifications or results from background
     * tracker HTTP work (including from background workers). Call once per frame
     * early in `SmatchetUI::Draw`.
     */
    /** @return true if a deferred notify was applied this call (live tracker request succeeded). */
    bool ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny();
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         const std::string& error);
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error);

    const std::vector<TrackerIssueTypeCreateMeta>& GetTrackerIssueTypeCreateMeta() const { return AvailableIssueTypeMeta; }

    // ---- Create issue flow -------------------------------------------------

    /**
     * Build a draft seeded from the last ticket currently displayed + the
     * configured TrackerConfig defaults. Safe to call from the UI thread.
     */
    IssueDraft BuildDraftFromLastTicket(const TrackerConfig& cfg) const;

    /**
     * Resolve the required-field set for a draft using cached createmeta.
     * Falls back to the hard minimum (project/issuetype/summary) if unknown.
     */
    RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                         const std::string& issueTypeName) const;

    /**
     * Fire-and-forget create. Seeds the cache with the new issue on success and
     * publishes a refreshed snapshot. Returns a future so bulk-import callers
     * can await per-row completions.
     */
    std::future<IssueCreateResult> CreateIssueAsync(const IssueDraft& draft);

    /**
     * Persist `draft` to SQLite and return the queued row id. Useful when the
     * user wants to stage creates before going online, or when a create fails
     * due to connectivity errors. Replayed by `TickOfflineCreates`.
     */
    std::int64_t QueueCreateOffline(const IssueDraft& draft);

    /**
     * Replay any queued offline creates. No-op when the queue is empty or the
     * backend is unreachable. Intended to be polled from the main tick.
     */
    void TickOfflineCreates();

    /** Current depth of the offline create queue (SQLite row count). */
    size_t GetPendingCreateCount() const;
    /** Active offline create rows (`pending_creates`), oldest first. */
    std::vector<PendingCreate> GetPendingCreates() const;
    size_t GetDeadPendingCreateCount() const;
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const;

    struct DeadLetterRestoreSummary {
        int Restored = 0;
        int Failed = 0;
    };
    /** Move selected dead-letter rows back to the active offline queue (attempts reset to 0). */
    DeadLetterRestoreSummary RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds);
    /** One-shot startup message from legacy max-attempt pending drop; empty if none. */
    std::string TakeLegacyPendingStartupBanner();

    struct DeadLetterDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    /** Permanently remove dead-letter rows by `pending_creates_dead.dead_id`. */
    DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds);

    struct PendingQueueDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    /** Permanently remove active offline-queue rows by `pending_creates.id`. */
    PendingQueueDeleteSummary DeletePendingCreates(const std::vector<std::int64_t>& pendingIds);

    /** Safe field families for offline-queued field edits (transport failures only). */
    static bool FieldEditSupportsOfflineQueue(const TrackerField& field);

    /**
     * Persist a tracker field payload for later replay when connectivity returns.
     * @param fieldsPayloadJson JSON object map (field id -> backend-specific value).
     */
    std::int64_t QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                       const std::string& fieldsPayloadJson, std::string& outError,
                                       const std::string& originalRichValue = std::string());

    /** Replay queued offline field edits (rate-limited; called from UI tick). */
    void TickOfflineFieldEdits();

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const;
    /// Replace the queued payload with a user-resolved version and clear the conflict flag.
    /// The edit will be retried on the next TickOfflineFieldEdits pass.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedMarkdown,
                                  const std::string& richKind);

    struct PendingFieldEditDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    PendingFieldEditDeleteSummary DeletePendingFieldEdits(const std::vector<std::int64_t>& ids);

    struct DeadFieldEditDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds);

    /**
     * Background-fetch issues by key (Jira search) and merge into the local cache.
     * Used so bulk-import update rows can show field diffs when keys are outside the current JQL.
     */
    void PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive = false);
    bool IsBulkImportPrefetchInFlight(const std::string& issueKey) const;

    const TrackerField* FindFieldById(const std::string& fieldId) const;

    /**
     * Per-issue tracker edit metadata: true if the field may be edited for this issue.
     *
     * For Jira, we handle special cases:
     * `status`: never allow direct edit via field editmeta (Jira does not list status
     * like a normal settable field; updates use transitions).
     * `priority`: if editmeta is loaded but omits `priority`, allow edit
     * (Jira omits it inconsistently).
     *
     * Returns true when editmeta is not loaded yet (optimistic) or for
     * non-Jira backends (e.g. Plane). After a failed editmeta fetch for an issue, returns false for fields not in the bypass list.
     * @param fieldMeta optional catalog row for fieldId (avoids lookup; same as nullptr + catalog).
     */
    bool CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                              const TrackerField* fieldMeta = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr) const;

    bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                         const std::vector<std::string>& rawValues, std::string& outError);
    bool SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult);

    /**
     * Build the Jira fields payload + optimistic display map without calling the network.
     * Used when a network save failed with a transport error and the edit should be queued offline.
     */
    bool TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                    std::string& outFieldsPayloadJson, std::string& outError);
    bool ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result, std::string& outError);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. */
    void WarmIssueEditMetaAsync(const std::string& issueId);

    bool FetchIssueWatchers(const std::string& issueKey, std::vector<TrackerUser>& outWatchers,
                            std::string& outError) const;

    bool FetchIssueVotes(const std::string& issueKey, std::vector<TrackerUser>& outVoters,
                         std::string& outError, int* outVoteCount = nullptr, bool* outHasVoted = nullptr,
                         bool* outVotersInResponse = nullptr) const;

    bool SearchUsersByQuery(const std::string& query, std::vector<TrackerUser>& outUsers, std::string& outError) const;

    bool AddIssueCommentPlain(const std::string& issueKey, const std::string& plainText, std::string& outError);

    bool SubmitWorklog(const std::string& issueId, const std::string& timeSpent,
                       const std::string& timeRemaining, const std::string& adjustEstimate,
                       const std::string& workDescription, const std::string& startedDate,
                       std::string& outError);

    bool AddIssueCommentBlameContext(const std::string& issueKey, const std::string& p4User,
                                         const std::string& functionName, const std::string& filePath, int lineNumber,
                                         const std::string& changelist, const std::string& date, bool approximated,
                                         const std::string& codeSnippet, std::string& outError);

    bool FetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                                 std::string& outError) const;

  private:
    std::unique_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerBackendFactory> backendFactory_; ///< Lazy-initialized in `Initialize` if not pre-set via `SetBackendFactory`.
    std::unique_ptr<ITrackerClient> Backend;
    /// Owns the offline-create / offline-field-edit replay queues and their dead-letter management.
    /// Constructed lazily in `Initialize`. Public AppController methods (`QueueCreateOffline`,
    /// `GetPendingCreates`, etc.) are thin delegators that forward to this service. See
    /// CODE_REVIEW.md §1.7 / §7 item 12.
    std::unique_ptr<OfflineQueueService> offlineQueue_;
    std::vector<CachedTicket> ActiveTickets;
    mutable std::shared_ptr<const std::vector<CachedTicket>> activeTicketsPublished_;
    std::atomic<std::uint64_t> ActiveTicketsRevision{0};
    std::atomic<std::uint64_t> TrackerFieldCatalogRevision{0};
    mutable std::mutex availableFieldsMutex_; ///< Guards AvailableFields writes (UI) vs. FindFieldById reads (workers).
    std::vector<TrackerField> AvailableFields;
    std::vector<TrackerComponent> AvailableComponents;
    std::vector<TrackerIssueTypeCreateMeta> AvailableIssueTypeMeta;
    std::string LastTrackerFieldCatalogError;
    std::string LastTrackerFieldCatalogWarning;
    std::string LastTrackerTicketSyncWarning;
    bool fieldCatalogEverLoaded_ = false;
    std::vector<std::function<void(const std::string&)>> AutomationLogSinks;
    std::function<void(const std::string&)> OpenUrlHandler;
    std::function<void()> CloseEmbeddedUiHandler;
    std::function<void()> RequestAppQuitHandler;
    AttachmentViewerHandler AttachmentViewerHandlerCallback;
    AttachmentPreviewHandler AttachmentPreviewHandlerCallback;
    AttachmentCollectionHandler AttachmentCollectionHandlerCallback;
    OpenFilePathsHandler OpenFilePathsHandlerCallback;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    sol::state lua;
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlers_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlersByDisplayName_;
    std::vector<McpToolDefinition> luaMcpTools_;
    mutable std::mutex luaMcpToolsMutex_;
    std::vector<std::pair<std::string, sol::protected_function>> luaWindows_;
    std::vector<std::pair<std::string, std::string>> luaTicketActions_;
    std::vector<std::pair<std::string, std::string>> luaGlobalActions_;
    mutable std::mutex fieldIconMapsMutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByFieldId_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByDisplayName_;

#endif
    /** Absolute path to the `Scripts` folder (trailing slash), or empty to use `Scripts/` relative to cwd. */
    std::string luaScriptsDirectory_;

    struct IssueEditMetaCache {
        bool loaded = false;
        /** Field id -> backend allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
    };

    mutable std::mutex editMetaMutex_;
    std::unordered_map<std::string, IssueEditMetaCache> issueEditMeta_;
    std::unordered_map<std::string, IssueEditMetaCache> issueTypeEditMeta_;
    std::unordered_set<std::string> issueEditMetaWarmupInFlight_;

    /**
     * @param issueTypeKeyOverride if non-null and non-empty, used instead of scanning `ActiveTickets`
     *        for issuetype (safe for background threads that captured the key on the UI thread).
     * @param configSnapshot if non-null, used instead of ConfigManager::Load() (e.g. snapshot from main thread
     *        or loaded before InitLua to avoid parsing smatchet_config.json after Lua init in release builds).
     */
    bool EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError = nullptr,
                                   const std::string* issueTypeKeyOverride = nullptr,
                                   const TrackerConfig* configSnapshot = nullptr);
    bool RefreshIssueEditMeta(const std::string& issueId, std::string* outError = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    /** @param trackerCfgForWorker credentials/settings copy for background fetch (never ConfigManager::Load inside worker). */
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker);
    void EnsureCatalogHistoryField();
    bool TryBuildFieldEditPayloadForNetwork(const std::string& issueId, const TrackerField& field,
                                            const std::vector<std::string>& rawValues,
                                            const std::string& originalEstimateSnapshot,
                                            const std::string& remainingEstimateSnapshot,
                                            const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
                                            std::unordered_map<std::string, std::string>& outDisplayValues,
                                            std::string& outError);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;
    void LaunchBackgroundTask(std::function<void()> task);
    void JoinBackgroundTasks();

    void DrainTrackerConnectivityProbeFuture();
    void ApplyTrackerConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                             const TrackerReachabilityProbeResult& r);
    bool IsConnectivityDegradedForProbeInterval(TrackerConnectivityState nextProbeState) const;
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now);
    static TrackerConnectivityState MapReachabilityProbeKind(TrackerReachabilityProbeKind k);

    void requestDeferredLiveTrackerBackendSuccessNotify_() const;
    void applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_();

    std::chrono::steady_clock::time_point nextTrackerConnectivityProbeAt_{};
    bool trackerConnectivityProbeInFlight_ = false;
    std::future<TrackerReachabilityProbeResult> trackerConnectivityProbeFuture_;
    TrackerConnectivityState lastTrackerConnectivityState_ = TrackerConnectivityState::Unknown;
    std::string lastTrackerConnectivityDiagnostic_;
    bool trackerConnectivityRecoveryPending_ = false;
    std::atomic<bool> fieldCatalogRefetchAfterLiveTicketSyncPending_{false};
    mutable std::atomic<bool> deferredLiveTrackerBackendSuccessNotify_{false};

    mutable std::mutex activeTicketsMutex_;
    std::atomic<bool> shuttingDown_{false};
    std::vector<std::thread> backgroundWorkers_;
    mutable std::mutex backgroundWorkersMutex_;

    // Offline-replay throttle + in-flight guards (UI thread + background workers — all accesses
    // under offlineReplayScheduleMutex_).
    mutable std::mutex offlineReplayScheduleMutex_;
    std::chrono::steady_clock::time_point nextOfflineReplayAt_ = std::chrono::steady_clock::now();
    bool offlineReplayInFlight_ = false;
    std::chrono::steady_clock::time_point nextOfflineFieldEditReplayAt_ = std::chrono::steady_clock::now();
    bool offlineFieldEditReplayInFlight_ = false;
    // legacyPendingStartupBanner_ moved to OfflineQueueService (Phase 1A of item 12 extraction).

    mutable std::mutex bulkImportPrefetchKeysMutex_;
    std::unordered_set<std::string> bulkImportPrefetchKeysInFlight_;

    PluginHost* runtimePluginHost_ = nullptr;

#if defined(SMATCHET_WITH_MCP)
    static constexpr size_t kMcpActivityLogMax = 100;
    mutable std::mutex mcpActivityMutex_;
    std::deque<std::string> mcpActivityLog_;
    /** `steady_clock` epoch offset in nanoseconds; 0 means no client HTTP activity yet. */
    std::atomic<std::uint64_t> mcpLastClientHttpActivityNs_{0};
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    struct AutomationJob {
        enum class Type {
            RunAutoScript,
            TicketAction,
            GlobalAction,
            RunFlatScript
        };
        Type type;
        std::string scriptPathOrActionName;
        std::vector<std::string> selectedIds;
        std::string targetIssueId;
    };
    mutable std::mutex automationJobMutex_;
    std::condition_variable automationJobCv_;
    std::deque<AutomationJob> automationJobs_;
    // Lifetime contract (see ~AppController and AutomationWorkerLoop):
    //   - The worker reads `this` indirectly via each iteration's `bgState["__smatchet_app"] = this`.
    //   - `bgState` is a stack-local per iteration; it never escapes the iteration's try-block.
    //   - `~AppController` flips `automationWorkerShuttingDown_` and `automationWorker_.join()` BEFORE
    //     any AppController member is destroyed. The join therefore provides both the
    //     happens-before barrier and the guarantee that no live `bgState` (and no `__smatchet_app`
    //     pointer reachable from worker code) survives into member destruction.
    std::thread automationWorker_;
    std::atomic<bool> automationWorkerShuttingDown_{false};
    void AutomationWorkerLoop();
    void RunAutomationJob(sol::state& state, sol::environment& env, const AutomationJob& job);
    std::vector<std::string> activeSetupScripts_;
#endif

  private:
    struct StreamingSyncState {
        // Promoted to atomic: read from the worker thread and written from the UI thread
        // (StartStreamingSync). All other accesses use the natural atomic conversions; no
        // explicit .load()/.store() needed for the simple equality checks below.
        std::atomic<std::uint64_t> RequestId{0};
        std::atomic<bool> Cancelled{false};
        std::atomic<bool> Superseded{false};
        std::atomic<bool> Active{false};
        std::atomic<bool> ActiveSessionRunning{false};
        std::thread WorkerThread;

        // Accumulators / results
        std::atomic<size_t> TotalFetchedCount{0};
        std::atomic<bool> FullSyncCompleted{false};
        // FetchError is a std::string so it cannot be atomic. All reads and writes MUST be
        // guarded by QueueMutex below. The worker thread writes via the qLock at the bottom
        // of the lambda; the UI thread reads/clears via lock_guard in TickStreamingApply +
        // setup/teardown paths.
        std::string FetchError;
        // Soft warning channel (e.g. PlaneClient pagination cap). Same QueueMutex contract as
        // FetchError above. Distinct from FetchError so the UI can surface the caveat without
        // suppressing the success notification or flipping connectivity state.
        std::string Warning;

        // Cache processing queue + FetchError / Warning serialization. UI thread and worker thread.
        mutable std::mutex QueueMutex;
        std::vector<std::vector<CachedTicket>> PendingBatches;

        // State tracking for stale row deletion
        std::unordered_set<std::string> KeepIds;
        std::vector<std::string> BackgroundStaleIds;
    };

    void CancelAndJoinActiveStreamingSync();
    void StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy);

    std::atomic<uint64_t> currentFetchRequestId_{0};
    StreamingSyncState activeStreamingSync_;

    bool hasPendingSyncRequest_ = false;
    TrackerConfig pendingConfig_;
    ViewsStore pendingViews_;

    std::string localCacheDbPath_;

    // isDeletingStale_ is read by IsStreamingSyncActive() (const, callable from any thread)
    // and written on the UI thread inside TickStreamingApply / CancelAndJoinActiveStreamingSync.
    // The atomic promotion is the simplest correctness fix; the remaining stale-delete state
    // (vectors + counters) is UI-thread-only by contract.
    std::atomic<bool> isDeletingStale_{false};
    std::vector<std::string> staleIdsToDelete_;
    size_t totalStaleToDelete_ = 0;
    size_t staleDeletedSoFar_ = 0;
};

#endif




