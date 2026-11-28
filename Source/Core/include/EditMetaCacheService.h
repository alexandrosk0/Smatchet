#pragma once

// EditMetaCacheService — owns the per-issue + per-scope tracker edit-metadata cache
// (`editMetaMutex_` + its containers) and the methods that load, refresh, invalidate,
// prune, and warm it. Extracted verbatim from `AppController` per the AppController god-object
// decomposition plan (Phase 1), mirroring the OfflineQueueService / TicketSyncService template:
// the service holds an `IEditMetaDeps&` (typically backed by `GridContextDepsAdapter`) and reaches
// AppController-side state only through that interface. AppController's public surface keeps the
// same shape but its bodies are thin delegators that forward into this service.
// Lifetime contract mirrors OfflineQueueService: AppController owns the service via
// `std::unique_ptr` and outlives it; background warm workers launched via
// `deps_.LaunchBackgroundTask` are joined in `~AppController` before the deps adapter dies.
// Quality Pillar 6 (offline-first): both caches are keyed by the backend namespace (two trackers
// never share an issue id's or a scope's permissions). A *scope* is one (project, issue type) pair; a
// fetched result stands for its scope only when it is *representative* (at least one editable field,
// EditMetaScopePure.h), so one uneditable issue (a Done one) never narrows its siblings. A representative
// result is saved to the lookup cache (kind `editmeta_project_type`; the legacy `editmeta_type` rows are
// deleted once per backend and never read). The saved rows are loaded once per backend by the warm, so
// edit permissions still apply while the tracker is unreachable, for at most 30 days. A restored entry
// answers CanEdit* but never stands in for a live fetch when the tracker is reachable.
// Per-row fetches are limited to backends whose answer varies per issue
// (ITrackerFieldCatalog::IssueEditMetaVariesPerIssue); the scope entry serves the others.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config/ConfigManager.h" // for TrackerConfig (by-value parameters + ConfigManager::Load)
#include "SerialTaskQueue.h"      // scopeSaveIo_: ordered, off-thread scope saves
#include "SmatchetResult.h"       // VoidResult (editmeta load/refresh — #21 outError → Result flip)
#include "StoreLoadLatch.h"

class IEditMetaDeps;
class ITrackerBackend;
class ITrackerFieldCatalog;
struct TrackerField;

class EditMetaCacheService {
  public:
    explicit EditMetaCacheService(IEditMetaDeps& deps);

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
     * non-Jira backends (e.g. Plane). After a failed editmeta fetch for an issue, returns false for fields not in the
     * bypass list.
     * @param fieldMeta optional catalog row for fieldId (avoids lookup; same as nullptr + catalog).
     */
    bool CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                              const TrackerField* fieldMeta = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr) const;

    /**
     * VoidResult: Ok on success (or optimistic no-op — no backend / empty issueId / cache hit / another
     * fetch of this issue already running, whose answer will land by itself, so Ok does not mean loaded);
     * Err(reason) when the editmeta fetch fails or is skipped because the tracker is offline (the
     * issue stays optimistic regardless — see impl).
     * @param issueTypeKeyOverride if non-null and non-empty, used instead of scanning `ActiveTickets`
     *        for issuetype (safe for background threads that captured the key on the UI thread).
     * @param configSnapshot if non-null, used instead of ConfigManager::Load() (e.g. snapshot from main thread
     *        or loaded before InitLua to avoid parsing smatchet_config.json after Lua init in release builds).
     */
    VoidResult EnsureIssueEditMetaLoaded(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr,
                                         const TrackerConfig* configSnapshot = nullptr);
    VoidResult RefreshIssueEditMeta(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr);

    // Pane-bound variants for a field edit (#2260): the edit's own backend, backend namespace and issue
    // type, never the focused pane's. An empty `issueTypeKey` skips the per-issue-type fallback.
    VoidResult EnsureIssueEditMetaLoadedFor(const std::shared_ptr<ITrackerBackend>& backend,
                                            const std::string& backendKey, const std::string& issueId,
                                            const std::string& issueTypeKey,
                                            const TrackerConfig* configSnapshot = nullptr);
    VoidResult RefreshIssueEditMetaFor(const std::shared_ptr<ITrackerBackend>& backend, const std::string& backendKey,
                                       const std::string& issueId, const std::string& issueTypeKey);
    bool CanEditFieldForIssueWithType(const std::string& backendKey, const std::string& issueId,
                                      const std::string& fieldId, const TrackerField* fieldMeta,
                                      const std::string& issueTypeKey) const;
    void InvalidateIssueEditMeta(const std::string& issueId);
    /// Same, for the tracker namespace `backendKey` (a pane that is not the focused one).
    void InvalidateIssueEditMeta(const std::string& backendKey, const std::string& issueId) {
        InvalidateIssueEditMetaFor(backendKey, issueId);
    }
    void PruneEditMetaCacheToActiveTickets();
    /** Load the saved per-type permissions once per backend, then fetch one representative issue per
     * issue type that has no live entry yet.
     * @param trackerCfgForWorker credentials/settings copy for background fetch (never ConfigManager::Load inside
     * worker). */
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. Called
     * every frame for the active row, so it does nothing while the tracker is offline and backs off
     * after a failed fetch (Quality Pillar 6). */
    void WarmIssueEditMetaAsync(const std::string& issueId);
    /// Connectivity came back: clear every failure backoff so the next warm retries.
    void OnConnectivityRecovered();
    /// Blocks until every queued scope save has run, or `timeout` passes (true: nothing left queued). Shutdown
    /// and tests only; a save a refused launch left queued runs on the caller.
    bool FlushScopeSaves(std::chrono::milliseconds timeout);

  private:
    struct IssueEditMetaCache {
        bool loaded = false;
        /// Fetched in this session. False for a per-type entry restored from the lookup cache.
        bool live = false;
        /** Field id -> backend allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
        /** After a failed fetch: the per-frame warmup waits until this time before trying again. */
        std::chrono::steady_clock::time_point retryAfter{};
    };
    /// Issue id -> editmeta, for one backend namespace.
    using EditMetaById = std::unordered_map<std::string, IssueEditMetaCache>;
    /// Backend namespace -> EditMetaById.
    using EditMetaByBackend = std::unordered_map<std::string, EditMetaById>;
    /// Lower-cased issue type -> editmeta, for one project.
    using ScopeByType = std::unordered_map<std::string, IssueEditMetaCache>;
    /// Project (ProjectOfIssue of the issue; "" when its id carries none) -> ScopeByType.
    using ScopeByProject = std::unordered_map<std::string, ScopeByType>;
    /// Backend namespace -> ScopeByProject.
    using ScopeByBackend = std::unordered_map<std::string, ScopeByProject>;

    /// How far a load may lean on the scope entry. ScopeSuffices: a live scope entry answers (the field-edit
    /// path). IssueOwn: the issue's own editmeta is fetched when the backend's answer varies per issue (the
    /// focused-row warm). Refresh: always fetch.
    enum class LoadMode { ScopeSuffices, IssueOwn, Refresh };

    /// Claim on one issue's in-flight fetch. `Gen` is unique per begun fetch: the result is kept only while it is
    /// still the one registered for `Key`, so a Refresh (a newer fetch) or an Invalidate (none) retires it.
    struct FetchTicket {
        std::string Key; ///< InFlightKey(backend, issue)
        std::uint64_t Gen = 0;
    };

    /// One scope the startup warm covers, with the tickets to try (at most kMaxCandidatesPerScope).
    struct ScopeCandidates {
        std::string Project;
        std::string IssueTypeKey;
        std::vector<std::string> IssueIds;
    };

    /// The loaded entry for (backendKey, id), or null. Caller holds editMetaMutex_.
    static const IssueEditMetaCache* FindLoaded(const EditMetaByBackend& maps, const std::string& backendKey,
                                                const std::string& id);
    /// The loaded entry for the scope, or null. Caller holds editMetaMutex_.
    static const IssueEditMetaCache* FindScopeLoaded(const ScopeByBackend& maps, const std::string& backendKey,
                                                     const std::string& project, const std::string& issueTypeKey);
    /// True and `project` set (ProjectOfIssue; "" when the id carries none) when an issue's result may be kept under
    /// a scope: it needs an issue type, and a project unless the backend's answer does not vary per issue (then an
    /// id without a project, such as a GitHub issue, gets one scope per type).
    static bool ScopeProjectFor(const std::string& issueId, const std::string& issueTypeKey, bool variesPerIssue,
                                std::string& project);
    static std::string InFlightKey(const std::string& backendKey, const std::string& issueId);
    /// True when the issue's own entry, or (per `mode`) a live scope entry, already answers: no fetch is needed.
    /// `project` is null when the issue has no scope.
    bool AnsweredFromCache(const std::string& backendKey, const std::string& issueId, const std::string* project,
                           const std::string& issueTypeKey, bool variesPerIssue, LoadMode mode) const;
    /// What one FetchIssueEditMeta call produced: the entry to keep on success, or the error and a failure marker.
    struct FetchResult {
        bool Ok = false;
        std::string Error;
        IssueEditMetaCache Cache;
    };
    static FetchResult FetchEditMeta(ITrackerFieldCatalog* catalog, const TrackerConfig& cfg,
                                     const std::string& issueId);
    /// Keep `fetched` per the rules of LoadIssueEditMeta (own entry, scope promotion, failure keeps loaded data) and
    /// retire `ticket`. True when the fetch was no longer current (nothing stored). `scopeSeq` is the number of the
    /// save the caller must now post (PostScopeSave), or 0 when none is due (not promoted, or the live scope entry
    /// already says the same). Takes editMetaMutex_.
    bool StoreFetchResult(const FetchTicket& ticket, const std::string& backendKey, const std::string& issueId,
                          const std::string* project, const std::string& issueTypeKey, bool variesPerIssue,
                          const FetchResult& fetched, bool promote, std::uint64_t& scopeSeq);
    /// Register a new fetch of `key` and return its ticket. False (nothing registered) when a fetch of `key` is
    /// already running and `mode` is not Refresh: a Refresh always begins and supersedes the running one.
    /// Caller holds editMetaMutex_.
    bool TryBeginIssueFetchLocked(const std::string& key, LoadMode mode, FetchTicket& out);
    /// True while `ticket` is the fetch registered for its key. Caller holds editMetaMutex_.
    bool IssueFetchCurrentLocked(const FetchTicket& ticket) const;
    /// Unregister `ticket`'s fetch, unless a newer one replaced it. Idempotent; any thread.
    void ReleaseIssueFetch(const FetchTicket& ticket);
    /// Fetch (or accept from the scope) the edit metadata of one issue; see LoadMode. `started` is a ticket the
    /// caller already began (the focused-row warm registers on the UI thread before launching); null begins one
    /// here. A fetch that is no longer current when it returns (superseded, invalidated) stores nothing. A
    /// representative result is promoted to its scope and saved. `refused` (optional) is set when no request was
    /// made because another fetch of this issue is running or `started` was superseded: the issue is neither loaded
    /// nor failed. Runs on a worker or a caller that may block on the network.
    VoidResult LoadIssueEditMeta(const std::shared_ptr<ITrackerBackend>& backend, const std::string& backendKey,
                                 const std::string& issueId, const std::string& issueTypeKey,
                                 const TrackerConfig* configSnapshot, LoadMode mode,
                                 const FetchTicket* started = nullptr, bool* refused = nullptr);
    /// Background-task body of WarmIssueTypeEditMetaAtStartAsync: try each scope's candidate tickets until one
    /// gives a representative result. Runs off the UI thread.
    void WarmIssueTypeEditMetaWorker(const std::vector<ScopeCandidates>& scopes,
                                     const std::shared_ptr<ITrackerBackend>& backend, const std::string& backendKey,
                                     const TrackerConfig& trackerCfgForWorker);
    /// Seed scopeEditMeta_ from the lookup cache once per backend, on a worker; never overrides an entry
    /// fetched meanwhile. Also deletes the backend's legacy `editmeta_type` rows once.
    void EnsureSavedTypesLoaded(const std::string& backendKey);
    /// Worker only: delete the backend's legacy per-type rows (once per backend per session).
    void PurgeLegacyTypeRows(const std::string& backendKey);
    /// Queue the save of one scope's permissions (a representative result) under the number StoreFetchResult gave
    /// it. Never blocks on SQLite: the write runs on `scopeSaveIo_`, in order, and a save superseded by a newer
    /// number for the same scope is skipped.
    void PostScopeSave(const std::string& backendKey, const std::string& project, const std::string& issueTypeKey,
                       const std::unordered_map<std::string, bool>& fieldCanEdit, std::uint64_t seq);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;
    void InvalidateIssueEditMetaFor(const std::string& backendKey, const std::string& issueId);
    /// CanEditFieldForIssue body. `explicitTypeKey` null → the issue type is resolved from the focused pane.
    bool CanEditFieldImpl(const std::string& backendKey, const std::string& issueId, const std::string& fieldId,
                          const TrackerField* fieldMeta, const std::string* explicitTypeKey) const;

    IEditMetaDeps& deps_;

    // editMetaMutex_ guards every container below up to (not including) `savedTypesLoad_` and
    // `scopeSaveIo_`, which synchronize themselves. mutable because CanEditFieldForIssue is const and only
    // reads the maps. It is a leaf lock: never held across a launch, a fetch or a lookup-cache call, so a
    // LaunchClaim release (which takes it) is safe inside any launcher.
    mutable std::mutex editMetaMutex_;
    EditMetaByBackend issueEditMeta_;
    ScopeByBackend scopeEditMeta_;
    /// InFlightKey(backend, issue) -> generation of the fetch now running for that issue.
    std::unordered_map<std::string, std::uint64_t> issueFetchInFlight_;
    std::uint64_t nextFetchGen_ = 0;
    /// InFlightKey(backend, scope store key) -> when a scope whose candidates gave no representative result
    /// may be tried again.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> scopeNoRepresentativeUntil_;
    /// Backends whose legacy `editmeta_type` rows were deleted.
    std::unordered_set<std::string> legacyPurged_;
    /// InFlightKey(backend, scope store key) -> the newest queued save's sequence number. A queued save whose
    /// number is no longer the newest is skipped: the newer one writes the same scope.
    std::unordered_map<std::string, std::uint64_t> latestScopeSeq_;
    std::uint64_t nextScopeSeq_ = 0;
    /// Backends whose saved scope rows were loaded (or are loading).
    smatchet::offline::StoreLoadLatch savedTypesLoad_;
    /// Scope saves, in order, off the calling thread. Declared last so it is destroyed first.
    smatchet::SerialTaskQueue scopeSaveIo_;
};
