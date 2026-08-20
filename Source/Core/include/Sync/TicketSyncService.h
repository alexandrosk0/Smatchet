#pragma once

// TicketSyncService — owns streaming ticket-sync state (worker thread, batch queue, supersede
// FSM, stale-deletion bookkeeping) and applies fetched batches to the local SQLite cache.
// Extracted from `AppController.cpp` per BACKLOG_CODE_REVIEW.md §1.7 / §7 item 11.
// As of Phase 1C all runtime methods + state live here. AppController's public surface keeps
// the same shape but its bodies are thin delegators (or de-inlined accessors that call into
// this service). Phase 2 (this PR) replaces `friend class TicketSyncService;` with the
// `ITicketSyncDeps` interface bundle (see ITicketSyncDeps.h); the service no longer touches
// AppController internals directly.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Sync/SyncTypes.h" // For TrackerIssueFetchPack (relocated out of AppController.h; core-include-dag Phase 3).
#include "Config/ConfigManager.h" // For TrackerConfig + ViewsStore (formerly via AppController.h).
#include "CachedTicketTypes.h"    // For CachedTicket inside StreamingSyncState (SQLite-free; ADR-0020).

class ITicketSyncDeps;

/// Lifetime contract mirrors `OfflineQueueService`: AppController owns the service via
/// `std::unique_ptr` and outlives it. The constructor stores an `ITicketSyncDeps&` reference
/// (typically backed by `GridContextDepsAdapter`); methods reach AppController-side state
/// (`Cache`, `Backend`, `ActiveTickets`, edit-meta caches, connectivity-probe state) through
/// that interface. Tests pass a `FakeTicketSyncDeps` so the service is exercisable in
/// pure-logic doctest builds.
class TicketSyncService {
  public:
    explicit TicketSyncService(ITicketSyncDeps& deps);

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

    /// Decide whether a completed full-sync that kept zero tickets should be treated as suspect
    /// and have its mass deletion skipped. A zero-keep full-sync cannot prove the cache is stale:
    /// a 200-with-empty-body glitch or a transiently-broken query is indistinguishable from a
    /// genuinely-emptied project. Returns true (skip the wipe) while there is something to lose
    /// and the empty result has not yet BOTH repeated `emptyWipeThreshold` times AND persisted for
    /// `minEmptyStreakElapsed`, so offline-available rows survive a transient blip while a truly-
    /// empty project still converges. Pure + static so the streaming path and `ApplyIssueFetchPack`
    /// share one decision and it is unit-testable. The elapsed floor is the #2143 fix — see the
    /// definition for why a count alone was satisfiable by two flaps seconds apart.
    static bool ShouldSkipMassDeletionOnEmptyFullSync(std::size_t keepCount, std::size_t cachedRowCount,
                                                      int consecutiveEmptyFullSyncs, int emptyWipeThreshold,
                                                      std::chrono::milliseconds emptyStreakElapsed,
                                                      std::chrono::milliseconds minEmptyStreakElapsed);

    /// Drop from `staleIds` every id another live grid pane still holds. The stale set is
    /// computed as "cached rows this session's query did not return", which over-claims when
    /// sibling panes sync different queries into the SAME backend-keyed cache namespace — each
    /// pane would delete the others' rows every cycle (see ITicketSyncDeps::
    /// TicketIdsRetainedByOtherContexts). Order of the surviving ids is preserved. Pure +
    /// static so both apply paths share one decision and it is unit-testable.
    static std::vector<std::string> FilterStaleIdsRetainedElsewhere(const std::vector<std::string>& staleIds,
                                                                    const std::vector<std::string>& retainedElsewhere);

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
        /// Transport-shaped FetchError — written with FetchError under QueueMutex at the same
        /// worker-side classification seam (N12 slice 1); readers copy both under the lock.
        bool FetchErrorTransient = false;
        std::string Warning;

        mutable std::mutex QueueMutex;
        std::vector<std::vector<CachedTicket>> PendingBatches;
        std::unordered_set<std::string> KeepIds;
        std::vector<std::string> BackgroundStaleIds;
    };

    void StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy);

    // StartStreamingSync phases. Each mutates the member FSM state in place; lock scopes match
    // the original inline body (same mutexes, same points).

    /// Normalize the requested tracker kind, swap the live backend when it differs, and on a
    /// cross-kind swap clear the in-memory ActiveTickets snapshot so stale items don't linger.
    void SwapBackendIfTrackerChanged(const TrackerConfig& cfgCopy);

    /// Background streaming-fetch worker body. Coordinates with the UI thread via FSM atomics
    /// + `QueueMutex`.
    void RunStreamingWorkerBody(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy);

    // --- TickStreamingApply phase helpers ------------------------------------------------
    // TickStreamingApply is a thin dispatcher over these per-phase steps. Each operates on the
    // member FSM state in place (no copies of the batch queue / ActiveTickets are introduced)
    // so the hot per-frame path keeps its original allocation + branch profile. The first three
    // yield a true result when the tick should return early, matching the original early-exit
    // control flow at those points.

    /// Phase 1a: if the active session was superseded, drain its queues, join the worker, reset
    /// FSM state, and kick off any pending sync request. Returns true if the tick must return.
    bool DiscardSupersededSessionIfNeeded();

    /// Phase 1b: when no session is busy, join a finished worker thread and start a pending sync
    /// request if one is queued. Returns true if the tick must return.
    bool StartPendingSyncIfIdle(bool isSessionBusy);

    /// Phase 2: progressive stale-ticket deletion within a per-frame budget (3 ms / 10 ids).
    /// Returns true if the tick must return (stale-deletion is in progress this frame).
    bool DrainStaleDeletionBudget();

    /// Phase 3: drain pending streaming batches into the cache + ActiveTickets within the
    /// per-frame budget (3 ms / 20 tickets), publishing the snapshot when state changed.
    void DrainPendingStreamingBatches();

    /// Phase 4: when the worker finished and the queue is drained, classify fetch error / soft
    /// warning, raise toasts, seed stale-deletion, and emit the coalesced Lua window bump.
    void FinalizeStreamingSessionIfDone();

    /// Phase 4b: seed the progressive stale-deletion queue from the session's background stale
    /// ids. Applies the empty-full-sync wipe guard and the sibling-pane subtraction, then arms
    /// `isDeletingStale_` when anything survives. No-op unless the session completed a full sync
    /// (a partial fetch cannot prove any row is gone). UI thread, once per session.
    void SeedStaleDeletionForSession(bool fullSyncCompleted, std::size_t keptThisSession);

    /// Phase 4c: converge this pane's in-memory roster onto exactly the ids its OWN query returned
    /// this session, then publish that set through `ITicketSyncDeps::PublishOwnedTicketIds` so a
    /// sibling pane's stale sweep can subtract it. The batch apply reads a cache namespace shared
    /// by every pane (ADR-0018 decision 4), so without this a pane can keep displaying rows only a
    /// sibling's query covers. No-op unless the session completed a full sync that kept at least
    /// one row. UI thread, once per session.
    void ConvergeActiveTicketsToSessionKeepSet(bool fullSyncCompleted, std::size_t keptThisSession);

    ITicketSyncDeps& deps_;
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

    // Tracks consecutive full syncs that returned zero tickets. A legitimately empty project
    // reconverges after kEmptyFullSyncWipeThreshold consecutive empty full-syncs AND
    // kEmptyFullSyncMinStreakElapsed of wall-clock, so stale cache rows are not silently
    // preserved forever but a burst of back-to-back empty ticks cannot satisfy the wipe either.
    int consecutiveEmptyFullSyncs_ = 0;
    static constexpr int kEmptyFullSyncWipeThreshold = 2;

    // Stamp of the FIRST empty full sync in the current streak; reset alongside the counter.
    // Steady (not system) clock so a wall-clock adjustment mid-streak cannot pass the floor early.
    std::chrono::steady_clock::time_point firstEmptyFullSyncAt_{};

    /// How long the empty condition must persist before a mass deletion is trusted (#2143) — long
    /// enough to outlast a burst of sync kicks, short enough to converge unnoticed.
    /// A function, not a `static constexpr` variable: this project builds at C++14
    /// (`CMAKE_CXX_STANDARD 14`), where such members are NOT implicitly inline, and binding one to
    /// the `std::chrono::duration` converting constructor's `const&` parameter ODR-uses it — which
    /// then links only with an out-of-line definition. `kEmptyFullSyncWipeThreshold` escapes that
    /// because an `int` passed by value is never ODR-used. Returning by value sidesteps it.
    static constexpr std::chrono::milliseconds EmptyFullSyncMinStreakElapsed() {
        return std::chrono::seconds(60);
    }

    /// Fold one completed fetch into the empty-streak counter + its stamp. Single seam so
    /// `TickStreamingApply` and `ApplyIssueFetchPack` cannot drift on how the streak is kept.
    void NoteFullSyncEmptiness(bool fullSyncCompleted, bool keptNothing);

    /// Wall-clock the current empty streak has persisted, or zero when no streak is open.
    std::chrono::milliseconds EmptyStreakElapsed() const;
};
