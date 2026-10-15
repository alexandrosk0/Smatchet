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
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Sync/ActiveRowMergePure.h" // ActiveRowIndex: the apply drain's id -> index map, kept per session.
#include "Sync/SyncMode.h"           // SyncMode: Delta (changed tickets only) vs Full (whole view).
#include "Sync/SyncSessionStats.h"   // SyncSessionStats - what the last session cost (sync.stats).
#include "Sync/SyncTypes.h" // For TrackerIssueFetchPack (relocated out of AppController.h; core-include-dag Phase 3).
#include "Config/ConfigManager.h" // For TrackerConfig + ViewsStore (formerly via AppController.h).
#include "SmatchetResult.h"       // Result<>
#include "TicketSyncMeta.h"       // TicketSyncMeta (the per-session version snapshot)
#include "CachedTicketTypes.h"    // For CachedTicket inside StreamingSyncState (SQLite-free; ADR-0020).
#include "Sync/RefusedKeysPure.h" // RefusedKeySet (the keys a tracker refused on a delta attempt)

struct TrackerError;             // Tracker/TrackerError.h — only named in the FetchHierarchyHop declaration
struct TrackerIssueFetchSummary; // ITrackerIssueReader.h — by-reference only (parent top-up seam)

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

    /// Cancels and joins the streaming worker. `IsActive()` drops to false at session finalize,
    /// one tick BEFORE `StartPendingSyncIfIdle` joins the finished worker, so an "idle" service
    /// can still own a joinable `std::thread`; destroying that member un-joined calls
    /// std::terminate (a hidden pane retired in that window crashed the app). Idle-only callers
    /// (`AppController::retireExpiredHiddenContexts_`) see an instant join.
    ~TicketSyncService();
    TicketSyncService(const TicketSyncService&) = delete;
    TicketSyncService& operator=(const TicketSyncService&) = delete;

    /// Cancel any in-flight streaming sync, join the worker thread, clear the pending-batches
    /// queue. Idempotent. Called from `~TicketSyncService`, `~AppController`, `EndCacheTeardown`
    /// and the start path of every new sync.
    void CancelAndJoinActiveStreamingSync();

    /// Cache-recreate gate, step 1 (UI thread): from here no new sync starts (`SyncWithBackend` only records the
    /// request, to run when the gate lifts; `TickStreamingApply` does nothing, so nothing reads or writes the
    /// cache), the running sync is flagged
    /// cancelled, and its worker thread is handed to the caller to join OFF the UI thread (a request on the wire
    /// runs to its own timeout). The returned thread is not joinable when no worker was running.
    /// A request queued behind the running session (`hasPendingSyncRequest_`) moves into the held slot, the one
    /// `EndCacheTeardown` replays or folds, so it does not also start a superseded session of its own; its mode
    /// combines with later held requests (Full wins).
    std::thread BeginCacheTeardown();

    /// Cache-recreate gate, step 2 (UI thread, after the thread from `BeginCacheTeardown` was joined and the cache
    /// was swapped): drops the cancelled session's batches and stale-deletion state, lifts the block, and replays
    /// the sync request recorded while it was up (if any) with its mode, so a pane opened or a views load that
    /// finished during the recreate still syncs. A no-op when the gate is not up.
    /// `foldIntoFollowUp` is for a caller that starts its own sync right after (the recreate's follow-up full
    /// refresh): the recorded request is not started here, only its mode is kept, and the next `SyncWithBackend`
    /// combines with it (Full wins) - so exactly one session runs after the recreate instead of a replay that the
    /// follow-up then cancels or duplicates.
    /// The caller must follow with a `SyncWithBackend` right away (AppController::FinishCacheRecreate_ does, in the
    /// same call). A fold nobody consumed is dropped by the next `BeginCacheTeardown`, so it
    /// cannot turn an unrelated later refresh into a Full.
    void EndCacheTeardown(bool foldIntoFollowUp = false);

    /// True between `BeginCacheTeardown` and `EndCacheTeardown`. Any thread.
    bool IsCacheTeardownActive() const { return startsBlocked_.load(); }

    /// Flag the in-flight streaming sync (if any) Cancelled + Superseded WITHOUT joining. The
    /// worker observes the flags at its next page / phase boundary, so a request already on the
    /// wire still runs to completion or timeout. Non-blocking, idempotent, any thread.
    void RequestCancel();

    /// Cancel then join every service in `services` in two passes: flag ALL first, then join
    /// each. A one-pass loop leaves the later workers un-cancelled while it waits on the earlier
    /// ones, so they start further requests and teardown costs up to one HTTP timeout PER pane;
    /// two passes let every worker unwind concurrently, bounding the total to about one timeout.
    /// Null entries and duplicates are skipped. Used by `~AppController` and
    /// `StartRecreateLocalCacheDatabase`.
    static void CancelAndJoinAll(const std::vector<TicketSyncService*>& services);

    /// TEST-ONLY SEAM: no production caller. Production applies fetches through the streaming path
    /// (`TickStreamingApply`); `AppController` has no passthrough. Kept because the stale-deletion and
    /// empty-full-sync-guard tests drive the one-shot apply directly. Do not add production callers.
    ///
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
    /// `mode` Delta (the default) lists the view's tickets with their versions and downloads only the changed
    /// ones, falling back to a full fetch on any failure; Full forces the whole-view download. Requests queued
    /// behind a running session combine so that Full wins (smatchet::sync::CombineSyncModes).
    /// With no `configOverride` or no `viewsOverride` the missing part (the config file, the saved views) is read
    /// first, on a worker (the file read and parse never run on the UI thread), and the sync then starts from a
    /// UI-thread post; the request is dropped if this service was destroyed meanwhile, or if a newer SyncWithBackend
    /// call (of any shape) was made while it read: the reads run on separate threads and their posts can land out of
    /// order, and the newest request wins. A dropped Full is kept (queued or folded into the next request).
    /// While the cache-recreate gate is up (`IsCacheTeardownActive`) nothing starts: the request is recorded instead
    /// (the latest config and views, the mode combined so that Full wins) and `EndCacheTeardown` replays it.
    void SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                         smatchet::sync::SyncMode mode = smatchet::sync::SyncMode::Delta);

    /// Count of streaming sessions started by this service. A change-monitor probe captures it when it
    /// is issued and drops its result when it moved: a sync that began in between writes newer rows
    /// than the probe saw. Any thread.
    std::uint64_t SessionEpoch() const { return sessionEpoch_.load(); }

    /// How many times the apply drain built its id -> index map from scratch (cumulative). A session builds it once
    /// and rebuilds only when the roster changed behind it; diagnostics and tests. UI thread.
    std::size_t MergeIndexBuilds() const { return mergeIndex_.Builds; }

    /// What the most recently finished session cost (`Valid` is false until one finishes). Any thread.
    smatchet::sync::SyncSessionStats LastSessionStats() const;

    /// True when any of (worker active, session running, stale-deletion in progress). Used by
    /// `AppController::IsStreamingSyncActive()` which de-inlines into a delegate.
    bool IsActive() const;

    /// `IsActive()`, or a saved-views read for a sync request is still running on a worker. That worker posts its
    /// result through `ITicketSyncDeps`, which belongs to the pane's context, so the pane must not retire (and free
    /// the service and, with it, the deps) while one is in flight. `AppController::retireExpiredHiddenContexts_`
    /// postpones on this, not on `IsActive()` (a views read is not a "sync running" for the status line). Any thread.
    bool HasPendingWork() const;

    /// Reset stale-deletion state to empty. Called from `StartRecreateLocalCacheDatabase` after the
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
    /// One queued group of rows for the UI-thread apply. `CachedUnchanged` rows were read back from the cache
    /// because the tracker reports them unchanged: the apply neither saves them nor carries comments forward,
    /// and only touches the in-memory roster when the row is missing or differs.
    struct SyncBatch {
        std::vector<CachedTicket> Rows;
        bool CachedUnchanged = false;
        /// Ids the session keeps WITHOUT queueing a row (a fetched row dropped because the cache holds a newer
        /// copy): the apply adds them to KeepIds so the roster convergence and the "N kept" count see them.
        std::vector<std::string> KeepOnlyIds;
    };
    /// Result of the delta attempt: `Finished` = the session's result was published (or the session was
    /// cancelled) and no full fetch is needed; otherwise `FallbackReason` says why Full must run.
    struct DeltaOutcome {
        bool Finished = false;
        std::string FallbackReason;
        /// The reason is an expected strategy choice (disabled, unsupported, nothing cached), not a failure.
        bool ExpectedFallback = false;
    };
    /// What a delta-aware hierarchy hop needs from the delta attempt: the cached versions it started from,
    /// the fields the view requires, and the age bound. Null = the hop downloads full bodies (a Full sync).
    struct HierarchyDelta {
        const std::unordered_map<std::string, TicketSyncMeta>* Meta = nullptr;
        std::vector<std::string> RequiredFields;
        std::int64_t NowSec = 0;
        std::int64_t MaxAgeSec = 0;
    };
    /// One hierarchy hop's rows: served from the cache, downloaded, and the keys the tracker refused.
    struct HopRows {
        std::vector<CachedTicket> Unchanged;
        std::vector<CachedTicket> Changed;
        std::vector<std::string> Refused;
        /// A key the roster listed as present came back with neither a row nor a refusal: the walk cannot
        /// be authoritative (nothing may be purged on its strength).
        bool Incomplete = false;
    };
    struct DeltaWork; // the roster, the fetch set and the rows of one delta attempt (defined in the .cpp)

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
        std::vector<SyncBatch> PendingBatches;
        std::unordered_set<std::string> KeepIds;
        std::vector<std::string> BackgroundStaleIds;
        /// Ids the view's own query returned this session; filled by PublishWorkerResult, guarded by QueueMutex.
        std::vector<std::string> ViewListedIds;

        /// Worker-filled part of the session cost record (mode, bodies, ...), guarded by QueueMutex.
        smatchet::sync::SyncSessionStats WorkerStats;
        /// Session clock + traffic baseline, written in StartStreamingSync before the worker is
        /// spawned and read on the UI thread at finalize.
        std::chrono::steady_clock::time_point SessionStartedAt{};
        smatchet::sync::TrackerTrafficSample TrafficAtStart;
    };

    void StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy, smatchet::sync::SyncMode mode);

    // StartStreamingSync phases. Each mutates the member FSM state in place; lock scopes match
    // the original inline body (same mutexes, same points).

    /// Normalize the requested tracker kind, swap the live backend when it differs, and on a
    /// cross-kind swap clear the in-memory ActiveTickets snapshot so stale items don't linger.
    void SwapBackendIfTrackerChanged(const TrackerConfig& cfgCopy);

    /// Background streaming-fetch worker body. Coordinates with the UI thread via FSM atomics
    /// + `QueueMutex`.
    void RunStreamingWorkerBody(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                smatchet::sync::SyncMode mode);

    /// Today's whole-view streamed fetch + parent/child top-ups, then publishes the session result.
    /// `fallbackReason` is non-empty when this run replaces an abandoned delta attempt.
    void RunFullFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                      const std::string& fallbackReason);

    /// Delta attempt: roster -> fetch set -> cached rows for the unchanged -> bodies for the changed ->
    /// queue + top-ups + publish. Never throws past the caller's fallback; see DeltaOutcome.
    DeltaOutcome RunDeltaFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy);
    /// Ends the session with a (non-transient) error and no purge, for a failure after rows were queued.
    void FailDeltaSession(std::uint64_t reqId, const std::string& message);

    /// Queues the delta rows, runs the hierarchy top-ups and publishes the session result.
    void FinishDeltaFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                          DeltaWork& work);

    /// Publishes a finished worker session (result flags, stats, stale ids) under QueueMutex; a no-op when
    /// the session was superseded or cancelled.
    /// `viewListedIds` are the ids the view's own query returned (not the hierarchy walk's rows); a completed
    /// session hands them to the deps (PublishViewListedIds) when it finalizes.
    void PublishWorkerResult(std::uint64_t reqId, const TrackerIssueFetchSummary& summary,
                             const std::unordered_set<std::string>& keepIds,
                             const smatchet::sync::SyncSessionStats& partialStats,
                             std::vector<std::string> viewListedIds);

    /// Appends rows to the apply queue; false when the session was superseded / cancelled meanwhile.
    /// A freshly fetched batch (`cachedUnchanged` false) is persisted HERE, on the worker, before it is
    /// queued - carried-forward comments, rows older than the cached version dropped, one SQLite transaction
    /// for the rest - so the UI-thread drain only touches memory (Pillar 2).
    bool QueueBatch(std::uint64_t reqId, std::vector<CachedTicket>&& rows, bool cachedUnchanged);

    /// Worker-side parent top-up (parent-issue-hierarchy plan): fetch every parent key the
    /// streamed batches referenced but did not contain, then chase THEIR parents too, one hop
    /// per round-trip, until a hop finds nothing new or the fetch-hop cap is hit — so a multi-
    /// level chain (epic -> story -> task -> subtask) resolves in this sync rather than one
    /// level per sync. Each hop is queued as its own extra batch. Runs only after a clean
    /// streamed fetch, never when cancelled, never when the active view hides parents, and
    /// never when `TrackerConfig::LoadParentIssues` is off. A failed keyed fetch is a
    /// `summary.Warning`, not a FetchError — the tickets fetched so far still apply.
    void FetchMissingParentsIntoQueue(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                      const std::vector<std::string>& parentRefs,
                                      std::unordered_set<std::string>& workerKeepIds, TrackerIssueFetchSummary& summary,
                                      const HierarchyDelta* delta = nullptr);

    /// One hop's rows for `keys` (parents by key, or children of `keys`). With `delta` it lists versions first
    /// (FetchRosterForKeys / FetchChildRosterOfKeys), serves unchanged rows from the cache and downloads only the
    /// changed ones; a roster error or an incomplete roster runs today's full-body hop instead. Without `delta`
    /// it is the full-body hop. A cancelled hop returns a `Cancelled` error.
    Result<HopRows, TrackerError> FetchHierarchyHop(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                                    const std::function<bool()>& cancelled,
                                                    const std::vector<std::string>& keys, bool childrenOfKeys,
                                                    const HierarchyDelta* delta);

    /// Downward mirror of FetchMissingParentsIntoQueue: given the keys the view's own JQL/filter
    /// actually streamed this sync, fetch their children (and their children's children, hop by
    /// hop) so a view like `key = EPIC-1` shows the epic's full descendant tree even though the
    /// filter itself only matched the epic. Runs only after a clean streamed fetch, never when
    /// cancelled, never when `TrackerConfig::LoadParentIssues` is off. Deliberately NOT gated on
    /// the active view's `HideParents` (unlike the ancestor fetch): hide-parents keeps exactly
    /// the leaf descendants this fetch supplies, so skipping it there would defeat the toggle
    /// rather than save work. A failed keyed fetch is a `summary.Warning`, not a FetchError — the
    /// tickets fetched so far still apply.
    void FetchChildrenIntoQueue(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                const std::vector<std::string>& streamedIds,
                                std::unordered_set<std::string>& workerKeepIds, TrackerIssueFetchSummary& summary,
                                const HierarchyDelta* delta = nullptr);

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

    /// Phase 4a: fold the worker's record, the session clock and the traffic delta into
    /// `lastStats_` and log it once. UI thread, once per session, before the stale-delete seeding.
    void RecordSessionStats(const std::string& fetchError, bool fullSyncCompleted);

    /// Phase 4c: converge this pane's in-memory roster onto exactly the ids its OWN query returned
    /// this session, then publish that set through `ITicketSyncDeps::PublishOwnedTicketIds` so a
    /// sibling pane's stale sweep can subtract it. The batch apply reads a cache namespace shared
    /// by every pane (ADR-0018 decision 4), so without this a pane can keep displaying rows only a
    /// sibling's query covers. No-op unless the session completed a full sync that kept at least
    /// one row. UI thread, once per session.
    void ConvergeActiveTicketsToSessionKeepSet(bool fullSyncCompleted, std::size_t keptThisSession);

    /// Phase 4d: hand the ids the view's own query returned to the deps once a COMPLETE session finished (a
    /// partial answer is not a listing of the view). UI thread, once per session.
    void PublishViewListing(bool fullSyncCompleted);

    ITicketSyncDeps& deps_;
    std::atomic<std::uint64_t> currentFetchRequestId_{0};
    std::atomic<std::uint64_t> sessionEpoch_{0};
    /// Raised by BeginCacheTeardown, lowered by EndCacheTeardown (the cache-recreate gate).
    std::atomic<bool> startsBlocked_{false};
    /// Cleared by the destructor; a worker's UI-thread post checks it before touching the service.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    /// Saved-views reads (SyncWithBackendLoadingViews_) still running on a worker; shared so the worker can lower it
    /// after the service is gone. Counted by HasPendingWork.
    std::shared_ptr<std::atomic<int>> viewsLoadsInFlight_ = std::make_shared<std::atomic<int>>(0);
    /// SyncWithBackend without config or views: load what is missing off the UI thread, then start (see
    /// SyncWithBackend).
    void SyncWithBackendLoadingViews_(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                                      smatchet::sync::SyncMode mode);
    /// The body of SyncWithBackend, without counting a new request (the UI-thread post of a finished files read is
    /// the request it was made for).
    void SyncWithBackendNow_(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                             smatchet::sync::SyncMode mode);
    /// A files read finished after a newer request was made: its sync is not started. A Full mode is kept (queued
    /// behind a running session, else folded into the next request) so dropping it never downgrades to Delta.
    void FoldDroppedRequest_(smatchet::sync::SyncMode mode);
    /// Requests made so far, bumped by every public SyncWithBackend call (UI thread only). A files read compares it
    /// with the value it was launched at when its post lands.
    std::uint64_t requestSeq_ = 0;
    mutable std::mutex statsMutex_;
    smatchet::sync::SyncSessionStats lastStats_; // guarded by statsMutex_
    StreamingSyncState activeStreamingSync_;

    bool hasPendingSyncRequest_ = false;
    smatchet::sync::SyncMode pendingMode_ = smatchet::sync::SyncMode::Delta;
    /// Mode of the session started last (UI thread only); a later Delta request must not downgrade it while
    /// its fetch is still running.
    smatchet::sync::SyncMode activeMode_ = smatchet::sync::SyncMode::Delta;
    /// Versions of the cached rows as of the session start; worker-thread only (written before the first
    /// batch is queued, read while persisting the batches of the same session).
    std::unordered_map<std::string, TicketSyncMeta> sessionMeta_;
    /// Keys the tracker refused to serve on an earlier delta attempt of this service. Each such attempt falls back
    /// to a full fetch; without this memory a key that stays refused would make every later delta attempt do the
    /// same. They are skipped - their cached row stands in, so the session still counts the key as returned and
    /// stays authoritative, and nothing is purged - until the memory is cleared by an
    /// explicit Full sync or a tracker switch, the key is served by a later fetch (the fallback full fetch
    /// included, at the version it was refused for or a newer one), it is older than DeltaSyncMaxRowAgeSec, or
    /// more than RefusedKeySet::MaxKeys() newer ones pushed it out. Worker-only during a session; cleared on the UI
    /// thread while no worker runs.
    smatchet::sync::RefusedKeySet refusedKeys_;
    /// The apply drain's id -> index map, kept for the whole session (UI thread).
    smatchet::sync::ActiveRowIndex mergeIndex_;
    /// Fills `sessionMeta_` from the cache (worker thread). Returns the error text when the read failed (the
    /// snapshot is then empty), empty on success.
    std::string LoadSessionMeta();
    TrackerConfig pendingConfig_;
    ViewsStore pendingViews_;

    /// The sync request that arrived while the cache-recreate gate was up, replayed by `EndCacheTeardown`. Latest
    /// config and views win (a null override stays null: the saved ones are read at replay); the mode combines, so
    /// a Full request is never downgraded by a later Delta one. UI thread only.
    struct BlockedSyncRequest {
        bool Has = false;
        bool HasConfig = false;
        TrackerConfig Config;
        bool HasViews = false;
        ViewsStore Views;
        smatchet::sync::SyncMode Mode = smatchet::sync::SyncMode::Delta;
    };
    BlockedSyncRequest blockedRequest_;
    /// The mode of a blocked request folded into the follow-up sync (`EndCacheTeardown(true)`): combined with the
    /// mode of the next `SyncWithBackend`, then cleared. UI thread only.
    bool hasFoldedMode_ = false;
    smatchet::sync::SyncMode foldedMode_ = smatchet::sync::SyncMode::Delta;
    void RecordBlockedSyncRequest_(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                                   smatchet::sync::SyncMode mode);

    // Stale-deletion bookkeeping. `isDeletingStale_` is atomic because `IsActive()` reads it
    // from `AppController::IsStreamingSyncActive()` which is callable from any thread. The
    // rest are UI-thread-only by contract.
    std::atomic<bool> isDeletingStale_{false};
    /// True while the background stale-row DELETE is running. Shared so the task can clear it without touching
    /// the service; it counts as busy, so no new session starts (and re-saves a row) before the delete lands.
    std::shared_ptr<std::atomic<bool>> deleteInFlight_ = std::make_shared<std::atomic<bool>>(false);
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
    static constexpr std::chrono::milliseconds EmptyFullSyncMinStreakElapsed() { return std::chrono::seconds(60); }

    /// Fold one completed fetch into the empty-streak counter + its stamp. Single seam so
    /// `TickStreamingApply` and `ApplyIssueFetchPack` cannot drift on how the streak is kept.
    void NoteFullSyncEmptiness(bool fullSyncCompleted, bool keptNothing);

    /// Wall-clock the current empty streak has persisted, or zero when no streak is open.
    std::chrono::milliseconds EmptyStreakElapsed() const;

    /// Cache key (tracker site and account, #2268) the live client was last synced for; empty
    /// until the first sync. A same-kind change of site or account recreates the client and
    /// clears the in-memory tickets when this differs.
    std::string lastAppliedCacheKey_;
};
