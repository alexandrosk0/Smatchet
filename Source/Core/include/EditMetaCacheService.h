#pragma once

// EditMetaCacheService — owns the per-issue + per-issue-type tracker edit-metadata cache
// (`editMetaMutex_` + its three containers) and the methods that load, refresh, invalidate,
// prune, and warm it. Extracted verbatim from `AppController` per the AppController god-object
// decomposition plan (Phase 1), mirroring the OfflineQueueService / TicketSyncService template:
// the service holds an `IEditMetaDeps&` (typically backed by `GridContextDepsAdapter`) and reaches
// AppController-side state only through that interface. AppController's public surface keeps the
// same shape but its bodies are thin delegators that forward into this service.
// Lifetime contract mirrors OfflineQueueService: AppController owns the service via
// `std::unique_ptr` and outlives it; background warm workers launched via
// `deps_.LaunchBackgroundTask` are joined in `~AppController` before the deps adapter dies.
// Quality Pillar 6 (offline-first): both caches are keyed by the backend namespace (two trackers
// never share an issue id's or an issue type's permissions), and every per-type result a fetch
// produces is saved to the lookup cache (kind `editmeta_type`). The saved rows are loaded once per
// backend by the warm, so edit permissions still apply while the tracker is unreachable. A restored
// entry answers CanEdit* but never stands in for a live fetch when the tracker is reachable.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config/ConfigManager.h" // for TrackerConfig (by-value parameters + ConfigManager::Load)
#include "SmatchetResult.h"       // VoidResult (editmeta load/refresh — #21 outError → Result flip)
#include "StoreLoadLatch.h"

class IEditMetaDeps;
class ITrackerBackend;
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
     * VoidResult: Ok on success (or optimistic no-op — no backend / empty issueId / cache hit);
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
    /// Issue id (or lower-cased issue type) -> editmeta, for one backend namespace.
    using EditMetaById = std::unordered_map<std::string, IssueEditMetaCache>;
    /// Backend namespace -> EditMetaById.
    using EditMetaByBackend = std::unordered_map<std::string, EditMetaById>;

    /// The loaded entry for (backendKey, id), or null. Caller holds editMetaMutex_.
    static const IssueEditMetaCache* FindLoaded(const EditMetaByBackend& maps, const std::string& backendKey,
                                                const std::string& id);
    static std::string InFlightKey(const std::string& backendKey, const std::string& issueId);
    /// Background-task body of WarmIssueTypeEditMetaAtStartAsync: load editmeta for the representative
    /// issues ({issue type, issue id} pairs). Runs off the UI thread.
    void WarmIssueTypeEditMetaWorker(const std::vector<std::pair<std::string, std::string>>& representatives,
                                     const std::shared_ptr<ITrackerBackend>& backend, const std::string& backendKey,
                                     const TrackerConfig& trackerCfgForWorker);
    /// Seed issueTypeEditMeta_ from the lookup cache once per backend, on a worker; never overrides an
    /// entry fetched meanwhile.
    void EnsureSavedTypesLoaded(const std::string& backendKey);
    /// Save one issue type's permissions to the lookup cache. Worker threads only (disk write).
    void SaveTypeEditMeta(const std::string& backendKey, const std::string& issueTypeKey,
                          const std::unordered_map<std::string, bool>& fieldCanEdit);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;
    void InvalidateIssueEditMetaFor(const std::string& backendKey, const std::string& issueId);
    /// CanEditFieldForIssue body. `explicitTypeKey` null → the issue type is resolved from the focused pane.
    bool CanEditFieldImpl(const std::string& backendKey, const std::string& issueId, const std::string& fieldId,
                          const TrackerField* fieldMeta, const std::string* explicitTypeKey) const;

    IEditMetaDeps& deps_;

    // editMetaMutex_ guards exactly the three containers below; the four move together as a unit.
    // mutable because CanEditFieldForIssue is const and only reads the maps.
    mutable std::mutex editMetaMutex_;
    EditMetaByBackend issueEditMeta_;
    EditMetaByBackend issueTypeEditMeta_;
    /// InFlightKey(backend, issue) of every running per-issue warmup.
    std::unordered_set<std::string> issueEditMetaWarmupInFlight_;
    /// Backends whose saved per-type rows were loaded (or are loading).
    smatchet::offline::StoreLoadLatch savedTypesLoad_;
};
