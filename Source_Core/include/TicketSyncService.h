#pragma once

// TicketSyncService — owns streaming ticket-sync state (worker thread, batch queue, supersede
// FSM, stale-deletion bookkeeping) and applies fetched batches to the local SQLite cache.
// Extracted from `AppController.cpp` per BACKLOG_CODE_REVIEW.md §1.7 / §7 item 11.
//
// As of Phase 1C all runtime methods + state live here. AppController's public surface keeps
// the same shape but its bodies are thin delegators (or de-inlined accessors that call into
// this service). Phase 2 will replace `friend class TicketSyncService;` with interface bundles.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AppController.h" // Needed for TrackerIssueFetchPack + ViewsStore + TrackerConfig.
#include "LocalCacheManager.h" // For CachedTicket inside StreamingSyncState.

/// Lifetime contract mirrors `OfflineQueueService`: AppController owns the service via
/// `std::unique_ptr` and outlives it. The constructor stores an `AppController&` back-reference
/// so methods can reach AppController-side state (`Cache`, `Backend`, `ActiveTickets`, edit-meta
/// caches, the connectivity-probe state, etc.) during the transition.
class TicketSyncService {
  public:
    explicit TicketSyncService(AppController& app);

    /// Cancel any in-flight streaming sync, join the worker thread, clear the pending-batches
    /// queue. Idempotent. Called from `~AppController`, `RecreateLocalCacheDatabase`, and the
    /// start path of every new sync.
    void CancelAndJoinActiveStreamingSync();

    /// Apply a single tracker-fetch result on the UI thread: persist fetched tickets to SQLite,
    /// classify any fetch error / soft warning, and prune cache rows that vanished from the
    /// remote when `FullSyncCompleted` is set.
    void ApplyIssueFetchPack(TrackerIssueFetchPack pack);

    /// Drain pending fetched batches into the cache + ActiveTickets, drive the supersede /
    /// cancel FSM, run progressive stale-deletion in 3-ms / 10-id frame budgets, and surface
    /// success / failure / warning toasts when the worker finishes. Called once per UI frame.
    void TickStreamingApply();

    /// Begin a tracker refresh. If a session is already busy (worker, apply queue, or
    /// stale-deletion), the request is deferred via `hasPendingSyncRequest_` and the active
    /// session is superseded. Otherwise transitions straight to `StartStreamingSync`.
    void SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride);

    /// True when any of (worker active, session running, stale-deletion in progress). Used by
    /// `AppController::IsStreamingSyncActive()` which de-inlines into a delegate.
    bool IsActive() const;

    /// Reset stale-deletion state to empty. Called from `RecreateLocalCacheDatabase` after the
    /// SQLite file is replaced so the service doesn't try to delete tickets from a fresh DB.
    void ResetStaleDeletionState();

  private:
    /// Streaming-sync FSM state. Moved out of AppController in Phase 1C of the item 11
    /// extraction. Worker thread + UI thread coordinate via the atomics + `QueueMutex`.
    struct StreamingSyncState {
        std::atomic<std::uint64_t> RequestId{0};
        std::atomic<bool> Cancelled{false};
        std::atomic<bool> Superseded{false};
        std::atomic<bool> Active{false};
        std::atomic<bool> ActiveSessionRunning{false};
        std::thread WorkerThread;

        std::atomic<size_t> TotalFetchedCount{0};
        std::atomic<bool> FullSyncCompleted{false};
        std::string FetchError;
        std::string Warning;

        mutable std::mutex QueueMutex;
        std::vector<std::vector<CachedTicket>> PendingBatches;
        std::unordered_set<std::string> KeepIds;
        std::vector<std::string> BackgroundStaleIds;
    };

    void StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy);

    AppController& app_;
    std::atomic<std::uint64_t> currentFetchRequestId_{0};
    StreamingSyncState activeStreamingSync_;

    bool hasPendingSyncRequest_ = false;
    TrackerConfig pendingConfig_;
    ViewsStore pendingViews_;

    // Stale-deletion bookkeeping. `isDeletingStale_` is atomic because `IsActive()` reads it
    // from `AppController::IsStreamingSyncActive()` which is callable from any thread. The
    // rest are UI-thread-only by contract.
    std::atomic<bool> isDeletingStale_{false};
    std::vector<std::string> staleIdsToDelete_;
    std::size_t totalStaleToDelete_ = 0;
    std::size_t staleDeletedSoFar_ = 0;
};
