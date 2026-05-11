#pragma once

// TicketSyncService — owns streaming ticket-sync state (worker thread, batch queue, supersede
// FSM, stale-deletion bookkeeping) and applies fetched batches to the local SQLite cache.
// Extracted from `AppController.cpp` per CODE_REVIEW.md §1.7 / §7 item 11.
//
// Phase 1A scope (this PR): class skeleton, owned by AppController as a `unique_ptr` member,
// and the two simplest methods migrated as the pattern (`CancelAndJoinActiveStreamingSync`,
// `ApplyIssueFetchPack`). All the heavy lifting — `TickStreamingApply` (~450 LOC),
// `SyncWithBackend`, `StartStreamingSync` (the worker thread spawn), the `StreamingSyncState`
// struct, and the stale-deletion state — stays on AppController in this PR.
//
// Future phases migrate the rest:
//   1B — `TickStreamingApply` (frame-budgeted UI tick) + stale-deletion state.
//   1C — `SyncWithBackend`, `StartStreamingSync` (worker spawn), `StreamingSyncState` struct.
//   2  — Replace `friend class TicketSyncService;` with interface bundles so the service is
//        independently testable without AppController-private access.

#include "AppController.h" // Needed for TrackerIssueFetchPack parameter type.

/// Lifetime contract mirrors `OfflineQueueService`: AppController owns the service via
/// `std::unique_ptr` and outlives it. The constructor stores an `AppController&` back-reference
/// so methods can reach the still-shared state during the transition. The `friend class
/// TicketSyncService;` declaration in `AppController.h` is the explicit access seam.
class TicketSyncService {
  public:
    explicit TicketSyncService(AppController& app);

    // --- Phase 1A: migrated methods ------------------------------------------------------
    /// Cancel any in-flight streaming sync, join the worker thread, clear the pending-batches
    /// queue. Idempotent. Called from `~AppController`, `RecreateLocalCacheDatabase`, and the
    /// start path of every new sync.
    void CancelAndJoinActiveStreamingSync();

    /// Apply a single tracker-fetch result on the UI thread: persist fetched tickets to SQLite,
    /// classify any fetch error / soft warning, and prune cache rows that vanished from the
    /// remote when `FullSyncCompleted` is set. Does not touch streaming-sync state directly.
    void ApplyIssueFetchPack(TrackerIssueFetchPack pack);

  private:
    AppController& app_;
};
