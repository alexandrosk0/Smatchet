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
#include "IssueDraft.h"
#include "IssueCreatePipeline.h"
#include "JiraClient.h"

#include <nlohmann/json.hpp>

/** Single consolidated Jira degraded/offline banner for main windows (replaces stacked warnings). */
struct JiraConnectivityBannerForUi {
    enum class Level { None, Warning, Error };
    Level Kind = Level::None;
    std::string Message;
};

class AppController {
  public:
    ~AppController();

    struct FieldEditResult {
        bool Ok = false;
        std::string Error;
        std::unordered_map<std::string, std::string> UpdatedDisplayValues;
    };

    void Initialize(const std::string& dbPath, const std::string& backendType);

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

    /**
     * Optional host callback for showing Jira attachments inside Unreal.
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

    void InitLua();

    void RunAutoScript(const std::string& scriptPath);

    /** Run a Lua file once (e.g. FieldDisplay.lua) to register UI hooks; errors go to automation log sinks. */
    void RunLuaSetupScript(const std::string& scriptPath);

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
    void SyncWithBackend(const JiraConfig* configOverride = nullptr, const ViewsStore* viewsOverride = nullptr);

    void RefreshLocalData();

    void UpdateTicket(const CachedTicket& ticket);

    std::vector<CachedTicket> GetActiveTickets() const;
    /** Cheap read: shared_ptr to last published ticket list (thread-safe with MCP / workers). */
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const;
    std::uint64_t GetActiveTicketsRevision() const { return ActiveTicketsRevision.load(); }

    /** Bumped when the field catalog changes (fetch, error clear, etc.); UI sort cache should invalidate. */
    std::uint64_t GetJiraFieldCatalogRevision() const { return JiraFieldCatalogRevision.load(); }
    std::uint64_t GetFieldCatalogRevision() const { return JiraFieldCatalogRevision.load(); }

    bool RefreshFieldCatalog(const JiraConfig& cfg);
    bool FetchFieldCatalog(const JiraConfig& cfg, TrackerFieldCatalogResult& outCatalog, std::string& outError) const;
    std::string BuildIssueBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) const;
    std::string BuildJqlSearchUrl(const JiraConfig& cfg, const std::string& jql) const;

    const std::vector<TrackerField>& GetAvailableFields() const { return AvailableFields; }
    const std::vector<TrackerComponent>& GetAvailableComponents() const { return AvailableComponents; }
    const std::string& GetFieldCatalogError() const { return LastJiraFieldCatalogError; }
    const std::string& GetFieldCatalogWarning() const { return LastJiraFieldCatalogWarning; }
    /** Set when a live JQL refresh failed with a transport-style error; UI may show cached tickets. */
    const std::string& GetLastTicketSyncWarning() const { return LastJiraTicketSyncWarning; }

    /**
     * One banner for field-catalog error/warning, ticket-list cache warning, and optional session note
     * (e.g. Views dashboard users-fetch warning). Prefer this over separate `GetFieldCatalogWarning` /
     * `GetLastTicketSyncWarning` lines in headers.
     */
    JiraConnectivityBannerForUi GetJiraConnectivityBannerForUi(const std::string* sessionCatalogNote = nullptr) const;

    /** Last outcome of periodic Jira /myself probe (UI thread). */
    enum class JiraConnectivityState {
        Unknown,
        AuthenticatedReachable,
        ReachableAuthOrConfigError,
        TransportDown,
        ServiceUnavailable,
    };
    /** Rate-limited background GET /myself; updates connectivity state and recovery latch. */
    void TickJiraConnectivityMonitor(const JiraConfig& cfg);
    /**
     * One-shot: true when reachability improved to authenticated-reachable (including from
     * transport-down, service-unavailable, or auth/config errors, and cold-start when a catalog
     * offline banner is still set). Clears ticket sync + field-catalog warnings and nudges
     * offline replay timers. UI should run catalog refetch + `SyncWithCurrentView` on the same frame.
     */
    bool ConsumeJiraConnectivityRecovery();
    /**
     * One-shot: true after a successful live `SyncWithBackend` issue fetch cleared a stale offline
     * field-catalog banner. UI should set `triggerCatalogRefetch` (same as connectivity recovery).
     */
    bool ConsumeFieldCatalogRefetchAfterLiveTicketSync();
    /**
     * Main-thread: applies connectivity + ticket/catalog banner updates latched after any successful
     * Jira HTTP work (including from background workers). Call once per frame early in `SmatchetUI::Draw`.
     */
    /** @return true if a deferred notify was applied this call (live Jira request succeeded). */
    bool ConsumeDeferredLiveJiraBackendSuccessNotifyIfAny();
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         const std::string& error);
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error);

    const std::vector<TrackerIssueTypeCreateMeta>& GetIssueTypeCreateMeta() const { return AvailableIssueTypeMeta; }

    // ---- Create issue flow -------------------------------------------------

    /**
     * Build a draft seeded from the last ticket currently displayed + the
     * configured JiraConfig defaults. Safe to call from the UI thread.
     */
    IssueDraft BuildDraftFromLastTicket(const JiraConfig& cfg) const;

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
     * Persist a Jira `fields` payload for later replay when connectivity returns.
     * @param fieldsPayloadJson JSON object map (field id -> Jira value), same shape as `UpdateIssueFields`.
     */
    std::int64_t QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                       const std::string& fieldsPayloadJson, std::string& outError);

    /** Replay queued offline field edits (rate-limited; called from UI tick). */
    void TickOfflineFieldEdits();

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const;

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
     * Per-issue Jira edit metadata: true if the field may be edited for this issue.
     * Matches SubmitFieldEdit: sprint fields, timetracking estimate columns, and `status` ignore
     * editmeta (Jira does not list status like a normal settable field; updates use transitions).
     * `priority`: if editmeta is loaded but omits `priority`, allow edit (Jira omits it inconsistently).
     * Returns true when editmeta is not loaded yet (optimistic) or for non-Jira backends.
     * After a failed editmeta fetch for an issue, returns false for fields not in the bypass list.
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

    /** Fetches watcher users for an issue (Jira only). */
    bool FetchIssueWatchers(const std::string& issueKey, std::vector<JiraUser>& outWatchers,
                            std::string& outError) const;

    bool JiraSearchUsersByQuery(const std::string& query, std::vector<JiraUser>& outUsers, std::string& outError) const;

    bool JiraAddIssueCommentPlain(const std::string& issueKey, const std::string& plainText, std::string& outError);

    bool JiraAddIssueCommentBlameContext(const std::string& issueKey, const std::string& p4User,
                                         const std::string& functionName, const std::string& filePath, int lineNumber,
                                         const std::string& changelist, const std::string& date, bool approximated,
                                         const std::string& codeSnippet, std::string& outError);

    bool JiraFetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                                 std::string& outError) const;

  private:
    std::unique_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerClient> Backend;
    JiraClient* JiraBackend = nullptr;
    std::vector<CachedTicket> ActiveTickets;
    mutable std::shared_ptr<const std::vector<CachedTicket>> activeTicketsPublished_;
    std::atomic<std::uint64_t> ActiveTicketsRevision{0};
    std::atomic<std::uint64_t> JiraFieldCatalogRevision{0};
    std::vector<TrackerField> AvailableFields;
    std::vector<TrackerComponent> AvailableComponents;
    std::vector<TrackerIssueTypeCreateMeta> AvailableIssueTypeMeta;
    std::string LastJiraFieldCatalogError;
    std::string LastJiraFieldCatalogWarning;
    std::string LastJiraTicketSyncWarning;
    bool fieldCatalogEverLoaded_ = false;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    sol::state lua;
#endif
    std::vector<std::function<void(const std::string&)>> AutomationLogSinks;
    std::function<void(const std::string&)> OpenUrlHandler;
    std::function<void()> CloseEmbeddedUiHandler;
    AttachmentViewerHandler AttachmentViewerHandlerCallback;
    AttachmentPreviewHandler AttachmentPreviewHandlerCallback;
    AttachmentCollectionHandler AttachmentCollectionHandlerCallback;
    OpenFilePathsHandler OpenFilePathsHandlerCallback;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlers_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayHandlersByDisplayName_;
#endif
    /** Absolute path to the `Scripts` folder (trailing slash), or empty to use `Scripts/` relative to cwd. */
    std::string luaScriptsDirectory_;

    struct IssueEditMetaCache {
        bool loaded = false;
        /** Field id -> Jira allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
    };

    mutable std::mutex editMetaMutex_;
    std::unordered_map<std::string, IssueEditMetaCache> issueEditMeta_;
    std::unordered_map<std::string, IssueEditMetaCache> issueTypeEditMeta_;
    std::unordered_set<std::string> issueEditMetaWarmupInFlight_;

    /**
     * @param issueTypeKeyOverride if non-null and non-empty, used instead of scanning `ActiveTickets`
     *        for issuetype (safe for background threads that captured the key on the UI thread).
     */
    bool EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError = nullptr,
                                   const std::string* issueTypeKeyOverride = nullptr);
    bool RefreshIssueEditMeta(const std::string& issueId, std::string* outError = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    void WarmIssueTypeEditMetaAtStartAsync();
    void EnsureCatalogHistoryField();
    bool TryBuildFieldEditPayloadForNetwork(const std::string& issueId, const TrackerField& field,
                                            const std::vector<std::string>& rawValues,
                                            const std::string& originalEstimateSnapshot,
                                            const std::string& remainingEstimateSnapshot,
                                            const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
                                            std::unordered_map<std::string, std::string>& outDisplayValues,
                                            std::string& outError);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;
    std::string ResolveLuaScriptPath(const std::string& filename) const;
    void LaunchBackgroundTask(std::function<void()> task);
    void JoinBackgroundTasks();

    void DrainJiraConnectivityProbeFuture();
    void ApplyJiraConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                          const JiraReachabilityProbeResult& r);
    bool IsConnectivityDegradedForProbeInterval(JiraConnectivityState nextProbeState) const;
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now);
    static JiraConnectivityState MapReachabilityProbeKind(JiraReachabilityProbeKind k);

    void requestDeferredLiveJiraBackendSuccessNotify_() const;
    void applyLiveJiraReachabilityAfterSuccessfulBackendRequest_();

    std::chrono::steady_clock::time_point nextJiraConnectivityProbeAt_{};
    bool jiraConnectivityProbeInFlight_ = false;
    std::future<JiraReachabilityProbeResult> jiraConnectivityProbeFuture_;
    JiraConnectivityState lastJiraConnectivityState_ = JiraConnectivityState::Unknown;
    std::string lastJiraConnectivityDiagnostic_;
    bool jiraConnectivityRecoveryPending_ = false;
    std::atomic<bool> fieldCatalogRefetchAfterLiveTicketSyncPending_{false};
    mutable std::atomic<bool> deferredLiveJiraBackendSuccessNotify_{false};

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
    std::string legacyPendingStartupBanner_;

    mutable std::mutex bulkImportPrefetchKeysMutex_;
    std::unordered_set<std::string> bulkImportPrefetchKeysInFlight_;
};

#endif