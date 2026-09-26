#pragma once

// IssueTransitionsCacheService — owns the per-issue transitions cache (Pillar 6 offline-first reads).
// Follows the same pattern as EditMetaCacheService: extracted from AppController to avoid god-object
// growth, mirrors the OfflineQueueService / TicketSyncService template. The service holds an
// `IEditMetaDeps&` (typically backed by `GridContextDepsAdapter`) and reaches AppController-side
// state only through that interface.
// Lifetime contract mirrors EditMetaCacheService: AppController owns the service via
// `std::unique_ptr` and outlives it; background workers launched via `deps_.LaunchBackgroundTask`
// are joined in `~AppController` before the deps adapter dies.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config/ConfigManager.h"
#include "KeyedLookupCache.h"
#include "SmatchetResult.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/TransitionsTypes.h"

class IEditMetaDeps;
class ILookupCache;

class IssueTransitionsCacheService {
  public:
    explicit IssueTransitionsCacheService(IEditMetaDeps& deps);

    /// Get available transitions for an issue + project + type combo. Transitions may be live (Fresh),
    /// learned from prior offline use, or absent; freshness indicates readiness. `applicable`
    /// is false when the backend doesn't support FetchIssueTransitions or the backend is unavailable.
    TransitionsLookup GetAvailableTransitions(const TransitionsQuery& q) const;

    /// Kick an async fetch of transitions if not already loaded for this issue.
    /// Non-blocking; the fetch runs in a background worker and populates the cache on completion.
    /// Applicability check returns immediately without logging if the backend doesn't support it.
    void EnsureIssueTransitionsLoaded(const TransitionsQuery& q, const TrackerConfig* configSnapshot = nullptr);

    /// Drop the cached entry for one issue (by issue id); called after a successful status transition
    /// so the next combo open re-fetches the now-current set of valid next statuses.
    void InvalidateIssueTransitions(const std::string& issueId);

    /// Call after connectivity recovers to resume fetches (Pillar 6).
    void OnConnectivityRecovered();

  private:
    static std::string LiveKey(const std::string& backendKey, const std::string& issueId);
    void EnsureLearnedLoaded(const std::string& backendKey);
    void RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                         const std::vector<TrackerFieldOption>& options, const std::shared_ptr<ILookupCache>& store);

    IEditMetaDeps& deps_;
    smatchet::offline::KeyedLookupCache<std::vector<TrackerFieldOption>> live_;
    mutable std::mutex learnedMutex_;
    std::unordered_map<std::string, std::vector<TrackerFieldOption>> learned_;
    std::unordered_set<std::string> learnedLoadedBackends_;
};
