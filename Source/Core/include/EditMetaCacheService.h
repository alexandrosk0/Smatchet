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
// #975: the warm worker captures the KICK-TIME `GridContextFieldCatalog*` (via
// `deps_.KickTimeFieldCatalog()` on the UI thread at kick time) and writes the per-project
// component-option maps under that catalog's OWN `availableFieldsMutex_` — a different mutex than
// `editMetaMutex_`. It never re-resolves the catalog at write time.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config/ConfigManager.h" // for TrackerConfig (by-value parameters + ConfigManager::Load)
#include "SmatchetResult.h"       // VoidResult (editmeta load/refresh — #21 outError → Result flip)

class IEditMetaDeps;
class ITrackerBackend;
struct TrackerField;
struct GridContextFieldCatalog;

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
     * Err(reason) when the editmeta fetch fails (the issue stays optimistic regardless — see impl).
     * @param issueTypeKeyOverride if non-null and non-empty, used instead of scanning `ActiveTickets`
     *        for issuetype (safe for background threads that captured the key on the UI thread).
     * @param configSnapshot if non-null, used instead of ConfigManager::Load() (e.g. snapshot from main thread
     *        or loaded before InitLua to avoid parsing smatchet_config.json after Lua init in release builds).
     */
    VoidResult EnsureIssueEditMetaLoaded(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr,
                                         const TrackerConfig* configSnapshot = nullptr);
    VoidResult RefreshIssueEditMeta(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    /** @param trackerCfgForWorker credentials/settings copy for background fetch (never ConfigManager::Load inside
     * worker). */
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. */
    void WarmIssueEditMetaAsync(const std::string& issueId);

  private:
    struct IssueEditMetaCache {
        bool loaded = false;
        /** Field id -> backend allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
    };

    /// Background-task body of WarmIssueTypeEditMetaAtStartAsync: load editmeta for the
    /// representative issues, then warm per-project component options. Runs off the UI thread.
    /// `catPtr` is the KICK-TIME context catalog captured by the caller (#975) — the worker must
    /// mutate THAT context's projectComponentsInFlight_ markers, not a completion-time re-resolve.
    void WarmIssueTypeEditMetaWorker(const std::vector<std::pair<std::string, std::string>>& representatives,
                                     const std::vector<std::string>& componentProjectKeys,
                                     const std::shared_ptr<ITrackerBackend>& backend, GridContextFieldCatalog* catPtr,
                                     TrackerConfig trackerCfgForWorker);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;

    IEditMetaDeps& deps_;

    // editMetaMutex_ guards exactly the three containers below; the four move together as a unit.
    // mutable because CanEditFieldForIssue is const and only reads the maps.
    mutable std::mutex editMetaMutex_;
    std::unordered_map<std::string, IssueEditMetaCache> issueEditMeta_;
    std::unordered_map<std::string, IssueEditMetaCache> issueTypeEditMeta_;
    std::unordered_set<std::string> issueEditMetaWarmupInFlight_;
};
