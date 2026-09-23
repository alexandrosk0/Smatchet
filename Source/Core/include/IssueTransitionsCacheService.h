#pragma once

// IssueTransitionsCacheService — owns the per-issue transitions cache (issueTransitionsMutex_ +
// its containers) and the methods that load, invalidate, and query it. Follows the same pattern
// as EditMetaCacheService: extracted from AppController to avoid god-object growth, mirrors
// the OfflineQueueService / TicketSyncService template. The service holds an `IEditMetaDeps&`
// (typically backed by `GridContextDepsAdapter`) and reaches AppController-side state only through
// that interface.
// Lifetime contract mirrors EditMetaCacheService: AppController owns the service via
// `std::unique_ptr` and outlives it; background workers launched via `deps_.LaunchBackgroundTask`
// are joined in `~AppController` before the deps adapter dies.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config/ConfigManager.h"
#include "SmatchetResult.h"

class IEditMetaDeps;
struct TrackerFieldOption;

struct TransitionsLookup {
    bool applicable = false; // false when the backend doesn't support FetchIssueTransitions
    bool loaded = false;     // true once the fetch has completed (only meaningful if applicable)
    std::vector<TrackerFieldOption> options;
};

class IssueTransitionsCacheService {
  public:
    explicit IssueTransitionsCacheService(IEditMetaDeps& deps);

    /// Get cached transitions for an issue. `applicable` is false when the backend doesn't support
    /// transitions (non-Jira) or when a fetch failed. `loaded` indicates whether a fetch has been
    /// attempted (only meaningful if applicable). `options` contains the available target statuses
    /// when applicable and loaded.
    TransitionsLookup GetAvailableTransitions(const std::string& issueId) const;

    /// Kick an async fetch of transitions if not already loaded for this issue.
    /// Non-blocking; the fetch runs in a background worker and populates the cache on completion.
    void EnsureIssueTransitionsLoaded(const std::string& issueId,
                                      const TrackerConfig* configSnapshot = nullptr);

    /// Drop the cached entry for one issue; called after a successful status transition
    /// so the next combo open re-fetches the now-current set of valid next statuses.
    void InvalidateIssueTransitions(const std::string& issueId);

  private:
    struct IssueTransitionsCache {
        bool applicable = false;
        bool loaded = false;
        std::vector<TrackerFieldOption> options;
    };

    IEditMetaDeps& deps_;
    mutable std::mutex issueTransitionsMutex_;
    std::unordered_map<std::string, IssueTransitionsCache> issueTransitions_;
};
