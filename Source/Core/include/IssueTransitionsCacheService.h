#pragma once

// IssueTransitionsCacheService — owns the per-issue transitions cache and the workflow remembered
// from earlier online use. Follows the same pattern as EditMetaCacheService: extracted from
// AppController to avoid god-object growth, mirrors the OfflineQueueService / TicketSyncService
// template. The service holds an `IEditMetaDeps&` (typically backed by `GridContextDepsAdapter`) and
// reaches AppController-side state only through that interface.
// Lifetime contract mirrors EditMetaCacheService: AppController owns the service via
// `std::unique_ptr` and outlives it; background workers launched via `deps_.LaunchBackgroundTask`
// are joined in `~AppController` before the deps adapter dies.
// Quality Pillar 6 (offline-first): a lookup never blocks on the network. Live fetches go through
// KeyedLookupCache (no fetch while offline, backoff after a failure, retry on reconnect), and every
// successful fetch is remembered per (project, issue type, from status) in the lookup cache so the
// status combo can offer valid moves while the tracker is unreachable.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config/ConfigManager.h"
#include "KeyedLookupCache.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/TransitionsTypes.h"

class IEditMetaDeps;
class ILookupCache;

class IssueTransitionsCacheService {
  public:
    explicit IssueTransitionsCacheService(IEditMetaDeps& deps);

    /// Transitions for the query without touching the network: the live set fetched this session,
    /// else the remembered workflow for (project, issue type, from status), else none. `applicable` is
    /// false when the active backend does not support transitions. Cheap; safe every frame.
    TransitionsLookup GetAvailableTransitions(const TransitionsQuery& q) const;

    /// Start a background fetch of the live transitions when one is allowed (not in flight, not
    /// offline, not inside the failure backoff) and load the remembered workflow once per backend.
    /// Non-blocking. Does nothing, and logs nothing, for a backend without transitions.
    void EnsureIssueTransitionsLoaded(const TransitionsQuery& q, const TrackerConfig* configSnapshot = nullptr);

    /// Drop the live entry for one issue; called after a successful status transition so the next
    /// combo open re-fetches the now-current set of valid next statuses.
    void InvalidateIssueTransitions(const std::string& issueId);

    /// Connectivity came back: clear every failure backoff so the next combo open retries.
    void OnConnectivityRecovered();

  private:
    static std::string LiveKey(const std::string& backendKey, const std::string& issueId);
    static std::string LearnedMemoryKey(const std::string& backendKey, const std::string& learnedKey);
    bool BackendSupportsTransitions() const;
    void EnsureLearnedLoaded(const std::string& backendKey);
    void RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                         const std::vector<TrackerFieldOption>& options, const std::shared_ptr<ILookupCache>& store);

    IEditMetaDeps& deps_;
    smatchet::offline::KeyedLookupCache<std::vector<TrackerFieldOption>> live_;
    mutable std::mutex learnedMutex_;
    /// LearnedMemoryKey(backend, "project|type|from") -> target statuses. Guarded by learnedMutex_.
    std::unordered_map<std::string, std::vector<TrackerFieldOption>> learned_;
    /// Backends whose stored rows were loaded (or are loading). Guarded by learnedMutex_.
    std::unordered_set<std::string> learnedLoadedBackends_;
};
