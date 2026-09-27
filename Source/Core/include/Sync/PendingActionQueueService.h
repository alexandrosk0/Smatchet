#pragma once

// PendingActionQueueService — the offline queue for tracker actions other than issue creates and
// field edits (Quality Pillar 6): comments today, worklogs and watch next. One generic queue keyed by
// kind (PendingActionTypes.h) in the pending_actions tables, next to OfflineQueueService's create /
// field-edit queues and driven the same way: a write made while the tracker is unreachable is saved
// at once and replayed on reconnect; online, a failed send that the tracker may still accept is saved
// too. Replay is exactly-once where the tracker lets us check: a comment whose send was interrupted
// is looked up before it is sent again (PendingActionPolicyPure.h).
//
// Threads: SubmitOrQueue runs on the caller's worker. Tick and the timer calls run on the UI thread.
// Replay and every SQLite read or write run on workers launched through IOfflineQueueDeps. The UI
// reads only Snapshot() — one atomic shared_ptr load, never SQLite. Lifetime: AppController owns the
// service and joins its background tasks before destroying it (OfflineQueueService's contract).

#include "PendingActionTypes.h"
#include "Types/ConnectivityTypes.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IOfflineQueueDeps;
class ISyncCache;
class ITrackerCollaboration;
struct TrackerConfig;
struct TrackerError;

class PendingActionQueueService {
  public:
    explicit PendingActionQueueService(IOfflineQueueDeps& deps);

    PendingActionQueueService(const PendingActionQueueService&) = delete;
    PendingActionQueueService& operator=(const PendingActionQueueService&) = delete;

    using SubmitOutcome = PendingActionSubmitResult;

    /// Worker-safe; blocks on the network when online. Routes by `connectivityAtKick` (RouteWrite):
    /// the Read-only preference rejects; offline the action is queued without a request; online it
    /// is sent, and a failure the tracker may still accept is queued (as `ambiguous` when the request
    /// may have landed). A rejected send returns Failed with the tracker's message.
    SubmitOutcome SubmitOrQueue(PendingActionKind kind, const std::string& issueKey, const std::string& payloadJson,
                                TrackerConnectivityState connectivityAtKick);

    /// UI thread, every frame. Loads the snapshot once a cache exists, then — when the snapshot holds
    /// a row this context can replay and the replay timer has passed — launches one replay pass.
    void Tick();

    /// Replay timers, driven like OfflineQueueService's: pushed forward while the transport is down,
    /// reset on reconnect and by an explicit "retry now".
    void PushReplayTimersForward(std::chrono::steady_clock::time_point pushTo);
    void RestartReplayTimersNow(std::chrono::steady_clock::time_point now);

    /// The queue as last read by a worker; never null. Any thread.
    std::shared_ptr<const PendingActionsSnapshot> Snapshot() const;

    /// Queue-panel actions. Each runs its SQLite work on a worker, then republishes the snapshot.
    void Discard(const std::vector<std::int64_t>& ids);
    void RestoreDead(const std::vector<std::int64_t>& originalIds);
    void DeleteDead(const std::vector<std::int64_t>& deadIds);
    /// A `needs_review` action the user confirmed did not land: back to `pending`, attempts reset. A
    /// row in any other state is left alone.
    void SendAgain(std::int64_t id);
    /// Reload the snapshot (e.g. after the local cache was recreated).
    void RequestSnapshotRefresh();

  private:
    struct ReplayTally {
        int Successes = 0;
        int Failures = 0;
        bool TransportDown = false; ///< a send failed on the network: the pass stops there
    };

    SubmitOutcome Enqueue(PendingActionKind kind, const std::string& issueKey, const std::string& payloadJson,
                          const char* state);
    /// One replay pass over this context's rows (worker). Returns the delay before the next pass.
    std::chrono::seconds RunReplayPass(ISyncCache& cache, ITrackerCollaboration& collab, const std::string& backendKey);
    void ReplayOne(ISyncCache& cache, ITrackerCollaboration& collab, const TrackerConfig& cfg,
                   const PendingActionRecord& row, ReplayTally& tally);
    /// An interrupted comment: true when it already reached the tracker and the row was retired, or
    /// when the check itself failed and was recorded; false when the comment should be sent now.
    bool ResolveAmbiguousComment(ISyncCache& cache, ITrackerCollaboration& collab, const PendingActionRecord& row,
                                 ReplayTally& tally);
    /// Record a failed send or check: archive when `retryState` is "" (final), else store it with one
    /// more attempt (archiving at the attempt cap).
    void RecordFailure(ISyncCache& cache, const PendingActionRecord& row, const TrackerError& error,
                       const char* retryState, ReplayTally& tally);
    void Archive(ISyncCache& cache, const PendingActionRecord& row, const char* reason, const std::string& detail);
    /// Run a queue-panel action on a worker, then republish.
    void RunCacheActionAsync(const char* what, std::function<void(ISyncCache&)> action);
    /// Reload the snapshot from `cache` and publish it (worker).
    void PublishSnapshot(ISyncCache& cache);
    /// First load (and reload after RequestSnapshotRefresh) on a worker, with a backoff after failure.
    void LoadSnapshotAsync(const std::shared_ptr<ISyncCache>& cache);

    IOfflineQueueDeps& deps_;

    std::shared_ptr<const PendingActionsSnapshot> snapshot_; ///< atomic_load / atomic_store only
    std::mutex publishMutex_; ///< one reload-and-publish at a time, so an older read never wins
    std::atomic<bool> snapshotLoaded_{false};
    std::atomic<bool> snapshotLoadInFlight_{false};

    mutable std::mutex scheduleMutex_;
    std::chrono::steady_clock::time_point nextReplayAt_ = std::chrono::steady_clock::now();
    bool replayInFlight_ = false;
    std::chrono::steady_clock::time_point nextSnapshotLoadAt_{};
};
