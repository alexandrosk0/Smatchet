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
#include "StoreLoadLatch.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/TransitionsTypes.h"

class IEditMetaDeps;
class ILookupCache;

class IssueTransitionsCacheService {
  public:
    explicit IssueTransitionsCacheService(IEditMetaDeps& deps);

    /// Transitions for the query without touching the network: the live set fetched this session for this
    /// issue FROM THE STATUS THE QUERY NAMES, else the remembered workflow for (project, issue type, from
    /// status), else none. A live set fetched for another status, or one the issue had moved on from, is never
    /// served as live. `applicable` is false when the active backend does not support transitions. Cheap; safe
    /// every frame.
    TransitionsLookup GetAvailableTransitions(const TransitionsQuery& q) const;

    /// Start a background fetch of the live transitions when one is allowed (not in flight, not
    /// offline, not inside the failure backoff) and load the remembered workflow once per backend. A live set
    /// is fetched again when it was fetched for another from-status than the query's, or is older than
    /// `lookup_live_max_age_sec` (0 = never); it stays on screen while that runs. Non-blocking. Does nothing,
    /// and logs nothing, for a backend without transitions.
    void EnsureIssueTransitionsLoaded(const TransitionsQuery& q, const TrackerConfig* configSnapshot = nullptr);

    /// Drop the live entry for one issue; called after a successful status transition so the next
    /// combo open re-fetches the now-current set of valid next statuses.
    void InvalidateIssueTransitions(const std::string& issueId);
    /// Same, for the issue in `backendKey`'s namespace: a status edit bound to a pane that may no longer
    /// have focus (#2260).
    void InvalidateIssueTransitions(const std::string& backendKey, const std::string& issueId);

    /// Bound the live map: when it holds more than `transitions_live_cache_max` entries, drop the live sets of
    /// issues no open pane lists any more. A set being fetched, and a failure record still backing off, are kept;
    /// under the cap this does nothing. Cheap enough to call after every ticket merge; any thread.
    void PruneLiveToActiveTickets();

    /// Connectivity came back: clear every failure backoff (fetches and the saved-workflow load) so the
    /// next combo open retries.
    void OnConnectivityRecovered();

  private:
    /// What a successful live fetch stores: the options and the question they answer.
    struct LiveTransitions {
        std::string IssueId;
        /// The from-status the query named when this was fetched (empty for an incomplete query).
        std::string FromStatusKey;
        /// The fetched targets include the from-status itself (no global or looped transition): the issue had
        /// moved on, so these belong to another status. Kept to stop a refetch loop, never served as live.
        bool ServerElsewhere = false;
        std::vector<TrackerFieldOption> Options;
    };
    /// What RememberLearned did with a fetched set.
    enum class LearnOutcome { Remembered, Erased, ServerElsewhere, Skipped };

    static std::string LiveKey(const std::string& backendKey, const std::string& issueId);
    static std::string LearnedMemoryKey(const std::string& backendKey, const std::string& learnedKey);
    bool BackendSupportsTransitions() const;
    void EnsureLearnedLoaded(const std::string& backendKey);
    LearnOutcome RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                                 const std::vector<TrackerFieldOption>& options,
                                 const std::shared_ptr<ILookupCache>& store);

    IEditMetaDeps& deps_;
    using LiveCache = smatchet::offline::KeyedLookupCache<LiveTransitions>;
    using LiveEntry = LiveCache::Entry;
    LiveCache live_;
    mutable std::mutex learnedMutex_;
    /// LearnedMemoryKey(backend, "project|type|from") -> target statuses. Guarded by learnedMutex_.
    std::unordered_map<std::string, std::vector<TrackerFieldOption>> learned_;
    /// Memory keys of edges erased this session because a fetch showed their from-status has no other move.
    /// The saved-workflow load may still be holding the row it read before the erase; it must not bring the edge
    /// back. Cleared per key when the edge is learned again. Guarded by learnedMutex_.
    std::unordered_set<std::string> learnedErased_;
    /// Backends whose stored rows were loaded (or are loading).
    smatchet::offline::StoreLoadLatch learnedLoad_;
};
