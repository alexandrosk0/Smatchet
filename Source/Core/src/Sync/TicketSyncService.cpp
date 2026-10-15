#include "TicketSyncService.h"

#include "AppController.h"
#include "CacheBackendKeyPure.h"
#include "ConfigManager.h"
#include "ITicketSyncDeps.h"
#include "ITrackerBackendFactory.h"
#include "ISyncCache.h"
#include "ITrackerIssueReader.h" // FetchIssuesForKeys + TrackerIssueFetchSummary (parent top-up)
#include "Logger.h"
#include "StringUtil.h"
#include "TimeNowPure.h"
#include "Tracker/TrackerError.h"                // TrackerError is only forward-declared by TicketSyncService.h
#include "Sync/RemoteVersionPure.h"              // never persist a row older than the cached version
#include "TicketSyncMeta.h"                      // BuildSyncMeta: the tracker version stored beside each saved row
#include "Sync/RefusalIsolatingFetchPure.h"      // refusal-isolating key-batch download (shared with the monitor)
#include "Sync/DeltaSyncPure.h"                  // strategy, fetch set, full-fetch threshold
#include "Sync/LazyEnrichmentCarryForwardPure.h" // keeps a lazily fetched comment thread across syncs
#include "Sync/TicketRosterFilterPure.h"         // shared keep-set roster filter (also used by the local-data refresh)
#include "Tracker/ParentHierarchyPure.h"         // ParentKeyOf — the one parent-field contract
#include "Views.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Quality Pillar 6: a synced row replaces the cached one wholesale, rich values included. Keep the
// comment thread and tooltip blob this session already holds when the row brings none and the
// comment count is unchanged (LazyEnrichmentCarryForwardPure.h), so the comments modal still has
// the thread offline. Only the matching rows' comment fields are copied, under the lock; the
// caller's save runs after it is released.
void CarryForwardLazyComments(ITicketSyncDeps& deps, std::vector<CachedTicket>& incoming) {
    std::unordered_set<std::string> ids;
    for (const CachedTicket& t : incoming) {
        if (!t.id.empty() && (t.GetFieldValueRef("comment").empty() ||
                              t.fieldRichValues.find(kCommentThreadRichKey) == t.fieldRichValues.end())) {
            ids.insert(t.id);
        }
    }
    if (ids.empty()) {
        return;
    }
    std::unordered_map<std::string, smatchet::sync::LazyCommentFields> previous;
    {
        std::lock_guard<std::mutex> lock(deps.ActiveTicketsMutex());
        for (const CachedTicket& t : deps.ActiveTickets()) {
            smatchet::sync::LazyCommentFields fields;
            if (ids.find(t.id) != ids.end() && smatchet::sync::SnapshotLazyCommentFields(t, fields)) {
                previous.emplace(t.id, std::move(fields));
            }
        }
    }
    for (CachedTicket& t : incoming) {
        const auto it = previous.find(t.id);
        if (it != previous.end()) {
            smatchet::sync::CarryForwardLazyCommentFields(it->second, t);
        }
    }
}

constexpr std::size_t kFetchedRowsPerFrame = 20;    // rows saved to SQLite per frame (3 ms budget)
constexpr std::size_t kUnchangedRowsPerFrame = 500; // cache-served rows: memory-only, so a bigger slice

// Merges one frame's rows into the in-memory roster (smatchet::sync::MergeRowsIntoActive: fresh rows always replace,
// cache-served rows only when missing or different) and, when anything changed, republishes the snapshot and bumps the
// revision under the same lock. The id -> index map in `index` lives for the whole session; recording the revision
// the bump produced keeps it valid for the next frame. Returns whether anything changed.
bool MergeRowsIntoActiveTickets(ITicketSyncDeps& deps, smatchet::sync::ActiveRowIndex& index,
                                const std::vector<CachedTicket>& fetched, const std::vector<CachedTicket>& unchanged,
                                std::vector<std::string>& movedIds) {
    std::lock_guard<std::mutex> lock(deps.ActiveTicketsMutex());
    std::vector<CachedTicket>& active = deps.ActiveTickets();
    const std::uint64_t revisionBefore = deps.ActiveTicketsRevisionNow();
    const bool changed =
        smatchet::sync::MergeRowsIntoActive(active, revisionBefore, index, fetched, unchanged, movedIds);
    if (changed) {
        deps.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>(active));
        deps.BumpActiveTicketsRevision();
        // The value our own bump produced. A bump another path made in between leaves the counter ahead of this,
        // so the next merge sees the mismatch and rebuilds instead of trusting a map that path may have outdated.
        index.Revision = revisionBefore + 1;
    }
    return changed;
}

// Drops the rows whose `updated` is provably OLDER than the version already cached for them (a late or
// reordered fetch must not roll a newer cached row back) and returns how many were dropped. A cached stamp more
// than a day in the future is unknown and never blocks. A Full sync is authoritative and does not call this. An
// unknown order (no stamp on either side) keeps the row, exactly as before versions were stored.
std::size_t DropRowsOlderThanCached(std::vector<CachedTicket>& rows,
                                    const std::unordered_map<std::string, TicketSyncMeta>& cachedMeta,
                                    std::vector<std::string>& droppedIds) {
    const std::size_t before = rows.size();
    const std::int64_t nowMs = TimeNowPure::NowUnixMs();
    rows.erase(
        std::remove_if(rows.begin(), rows.end(),
                       [&cachedMeta, &droppedIds, nowMs](const CachedTicket& row) {
                           const auto it = cachedMeta.find(row.id);
                           const bool stale = it != cachedMeta.end() &&
                                              smatchet::sync::IsIncomingRowStale(row.GetFieldValueRef("updated"),
                                                                                 it->second.RemoteUpdated, nowMs);
                           if (stale) {
                               LOG_DEBUG("TicketSyncService: dropped stale fetched row id=%s fetched=%s cached=%s",
                                         row.id.c_str(), row.GetFieldValueRef("updated").c_str(),
                                         it->second.RemoteUpdated.c_str());
                               droppedIds.push_back(row.id); // still part of the view: kept, not queued
                           }
                           return stale;
                       }),
        rows.end());
    return before - rows.size();
}

// The cached copies of rows a fetch lost to a newer cached version (DropRowsOlderThanCached). The fetched copy is
// not persisted, but the pane still has to show the row: a pane that does not hold it yet (a fresh pane, or one that
// evicted it) would otherwise lack it until the next sync. A failed read costs only that: the ids stay kept.
std::vector<CachedTicket> ReadCachedCopies(ISyncCache* cache, const std::string& backendKey,
                                           const std::vector<std::string>& ids) {
    if (cache == nullptr || ids.empty()) {
        return std::vector<CachedTicket>();
    }
    try {
        return cache->GetTicketsByIds(backendKey, ids);
    } catch (const std::exception& ex) {
        LOG_WARN("TicketSyncService: could not read %zu cached row(s) newer than the fetched copy: %s", ids.size(),
                 ex.what());
    } catch (...) {
        LOG_WARN("TicketSyncService: could not read %zu cached row(s) newer than the fetched copy (unknown exception)",
                 ids.size());
    }
    return std::vector<CachedTicket>();
}

// Saves `rows` and, beside each, the tracker version it was fetched at (delta-sync meta), in one
// transaction. A row without an `updated` stamp is saved with no meta (it will always be refetched).
// `overwriteOlder` is a Full sync's authority over the version guard.
void SaveRowsWithMeta(ISyncCache& cache, const std::string& backendKey, const std::vector<CachedTicket>& rows,
                      bool overwriteOlder) {
    smatchet::sync::SaveFetchedRows(cache, backendKey, rows, overwriteOlder);
}

} // namespace

TicketSyncService::TicketSyncService(ITicketSyncDeps& deps) : deps_(deps) {}

TicketSyncService::~TicketSyncService() {
    alive_->store(false);
    CancelAndJoinActiveStreamingSync();
}

bool TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(std::size_t keepCount, std::size_t cachedRowCount,
                                                              int consecutiveEmptyFullSyncs, int emptyWipeThreshold,
                                                              std::chrono::milliseconds emptyStreakElapsed,
                                                              std::chrono::milliseconds minEmptyStreakElapsed) {
    if (keepCount > 0) {
        return false;
    }
    if (cachedRowCount == 0) {
        return false;
    }
    if (consecutiveEmptyFullSyncs < emptyWipeThreshold) {
        return true;
    }
    // Count reached, but a count is only evidence if the empty condition actually PERSISTED
    // (#2143). Sync kicks can land back-to-back — pane focus changes, view re-syncs, a
    // connectivity-recovery refresh — so the threshold is reachable in seconds, which is how a
    // query that flapped empty twice deleted 23 offline rows that the next fetch brought back.
    return emptyStreakElapsed < minEmptyStreakElapsed;
}

void TicketSyncService::NoteFullSyncEmptiness(bool fullSyncCompleted, bool keptNothing) {
    if (fullSyncCompleted && keptNothing) {
        ++consecutiveEmptyFullSyncs_;
        if (consecutiveEmptyFullSyncs_ == 1) {
            // Opening stamp of this streak. Only the FIRST empty is stamped: the floor measures how
            // long the empty condition has held, not the gap between the last two samples.
            firstEmptyFullSyncAt_ = std::chrono::steady_clock::now();
        }
        return;
    }
    if (!keptNothing) {
        // Any non-empty result contradicts the streak outright — drop both the count and the stamp
        // so the next empty starts a fresh window rather than inheriting an old one.
        consecutiveEmptyFullSyncs_ = 0;
        firstEmptyFullSyncAt_ = std::chrono::steady_clock::time_point{};
    }
}

std::chrono::milliseconds TicketSyncService::EmptyStreakElapsed() const {
    if (consecutiveEmptyFullSyncs_ <= 0 || firstEmptyFullSyncAt_ == std::chrono::steady_clock::time_point{}) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 firstEmptyFullSyncAt_);
}

std::vector<std::string>
TicketSyncService::FilterStaleIdsRetainedElsewhere(const std::vector<std::string>& staleIds,
                                                   const std::vector<std::string>& retainedElsewhere) {
    if (retainedElsewhere.empty()) {
        return staleIds;
    }
    std::unordered_set<std::string> retained(retainedElsewhere.begin(), retainedElsewhere.end());
    std::vector<std::string> kept;
    kept.reserve(staleIds.size());
    std::copy_if(staleIds.begin(), staleIds.end(), std::back_inserter(kept),
                 [&retained](const std::string& id) { return retained.find(id) == retained.end(); });
    return kept;
}

void TicketSyncService::RequestCancel() {
    activeStreamingSync_.Cancelled = true;
    activeStreamingSync_.Superseded = true;
}

void TicketSyncService::CancelAndJoinAll(const std::vector<TicketSyncService*>& services) {
    std::vector<TicketSyncService*> unique;
    unique.reserve(services.size());
    for (TicketSyncService* svc : services) {
        if (svc && std::find(unique.begin(), unique.end(), svc) == unique.end()) {
            unique.push_back(svc);
        }
    }
    LOG_DEBUG("TicketSyncService::CancelAndJoinAll: cancelling and joining %zu service(s)", unique.size());
    for (TicketSyncService* svc : unique) {
        svc->RequestCancel();
    }
    for (TicketSyncService* svc : unique) {
        svc->CancelAndJoinActiveStreamingSync();
    }
}

void TicketSyncService::CancelAndJoinActiveStreamingSync() {
    RequestCancel();

    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }

    activeStreamingSync_.Active = false;
    activeStreamingSync_.ActiveSessionRunning = false;

    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();
    }
}

std::thread TicketSyncService::BeginCacheTeardown() {
    startsBlocked_.store(true);
    hasFoldedMode_ = false; // a fold left by an earlier recreate whose follow-up never ran must not ride on this one
    if (hasPendingSyncRequest_) {
        // A request queued behind the running session before the gate went up: the held slot owns it from here, so
        // the fold or the replay in EndCacheTeardown is the only way it runs (not a superseded start of its own, or a
        // second session after the follow-up). Full stays sticky through the combine.
        RecordBlockedSyncRequest_(&pendingConfig_, &pendingViews_, pendingMode_);
        hasPendingSyncRequest_ = false;
        pendingMode_ = smatchet::sync::SyncMode::Delta;
    }
    RequestCancel();
    return std::move(activeStreamingSync_.WorkerThread);
}

void TicketSyncService::EndCacheTeardown(bool foldIntoFollowUp) {
    if (!startsBlocked_.load()) {
        return; // the gate is not up (the same service listed twice): do not cancel a session started since
    }
    CancelAndJoinActiveStreamingSync(); // the worker was handed out and joined: nothing left to wait for
    ResetStaleDeletionState();
    startsBlocked_.store(false);
    if (!blockedRequest_.Has) {
        return;
    }
    // A request arrived while the gate was up (a pane opened mid-recreate, a views load that finished meanwhile):
    // run it now, with the mode it asked for, instead of losing it.
    BlockedSyncRequest request = std::move(blockedRequest_);
    blockedRequest_ = BlockedSyncRequest();
    if (foldIntoFollowUp) {
        // The caller syncs right after this returns: keep only the mode (Full stays sticky), so one session runs.
        foldedMode_ = request.Mode;
        hasFoldedMode_ = true;
        LOG_INFO("TicketSyncService: the sync requested during the cache recreate rides on the follow-up sync "
                 "(mode=%s).",
                 request.Mode == smatchet::sync::SyncMode::Full ? "full" : "delta");
        return;
    }
    LOG_INFO("TicketSyncService: replaying the sync requested during the cache recreate (mode=%s).",
             request.Mode == smatchet::sync::SyncMode::Full ? "full" : "delta");
    SyncWithBackend(request.HasConfig ? &request.Config : nullptr, request.HasViews ? &request.Views : nullptr,
                    request.Mode);
}

void TicketSyncService::RecordBlockedSyncRequest_(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                                                  smatchet::sync::SyncMode mode) {
    blockedRequest_.Mode = blockedRequest_.Has ? smatchet::sync::CombineSyncModes(blockedRequest_.Mode, mode) : mode;
    blockedRequest_.Has = true;
    blockedRequest_.HasConfig = configOverride != nullptr;
    if (configOverride != nullptr) {
        blockedRequest_.Config = *configOverride;
    }
    blockedRequest_.HasViews = viewsOverride != nullptr;
    if (viewsOverride != nullptr) {
        blockedRequest_.Views = *viewsOverride;
    }
}

void TicketSyncService::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (!deps_.Backend() || !deps_.Cache()) {
        LOG_WARN("TicketSyncService::ApplyIssueFetchPack skipped: backend=%d cache=%d", deps_.Backend() ? 1 : 0,
                 deps_.Cache() ? 1 : 0);
        return;
    }

    deps_.SetLastTrackerTicketSyncWarning(std::string(), /*transient=*/false);

    const std::vector<CachedTicket>& freshTickets = pack.Tickets;
    const std::string& fetchError = pack.FetchError;
    const std::string& fetchWarning = pack.Warning;
    const bool fullSyncCompleted = pack.FullSyncCompleted;

    // The pack carries its transport classification from the composition seam (N12 slice 1) —
    // do not re-classify the text here.
    if (!fetchError.empty() && pack.FetchErrorTransient) {
        deps_.SetLastTrackerTicketSyncWarning("Showing cached issues — live refresh did not complete: " + fetchError,
                                              /*transient=*/true);
        LOG_WARN("TicketSyncService::ApplyIssueFetchPack transport-style fetch issue: %s", fetchError.c_str());
        deps_.SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState::TransportDown);
        const auto nowProbe = std::chrono::steady_clock::now();
        deps_.SetNextTrackerConnectivityProbeAt(nowProbe);
        deps_.PushOfflineReplayTimersDuringTransportOutage(nowProbe);
    } else if (fetchError.empty()) {
        // Soft warnings still count as success — the fetched data is valid, just partial.
        if (!fetchWarning.empty()) {
            deps_.SetLastTrackerTicketSyncWarning("Sync completed with a caveat: " + fetchWarning,
                                                  /*transient=*/false);
            LOG_WARN("TicketSyncService::ApplyIssueFetchPack soft warning: %s", fetchWarning.c_str());
        }
        deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    }

    // Latch the namespace once per apply — the key only changes on a backend swap, which
    // happens on this (UI) thread between sessions (multi-grid Slice 1b).
    const std::string backendKey = deps_.CacheBackendKey();
    CarryForwardLazyComments(deps_, pack.Tickets);
    // An inline pack is not a Full sync session (activeMode_ belongs to the last streaming session, not this
    // pack): keep the older-version guard.
    SaveRowsWithMeta(*deps_.Cache(), backendKey, freshTickets, /*overwriteOlder=*/false);
    const size_t saved = freshTickets.size();

    size_t deleted = 0;
    // Empty-fetch guard: a full-sync that returns zero tickets cannot prove the cache is
    // stale — treating it as authoritative would wipe every row silently (e.g. a 200-with-
    // empty-body network glitch). Once the empty result has repeated kEmptyFullSyncWipeThreshold
    // times AND persisted EmptyFullSyncMinStreakElapsed() we trust it, so genuinely-empty projects
    // converge eventually without a burst of back-to-back empties qualifying (#2143).
    NoteFullSyncEmptiness(fullSyncCompleted, freshTickets.empty());
    if (fullSyncCompleted) {
        std::unordered_set<std::string> keepIds;
        keepIds.reserve(freshTickets.size());
        for (const auto& t : freshTickets) {
            if (!t.id.empty()) {
                keepIds.insert(t.id);
            }
        }
        std::vector<CachedTicket> existing = deps_.Cache()->GetAllTickets(backendKey);
        // Single source of truth for the wipe decision — the same helper the streaming path uses,
        // so the empty-full-sync data-loss guard can never drift between the two apply paths.
        if (!ShouldSkipMassDeletionOnEmptyFullSync(keepIds.size(), existing.size(), consecutiveEmptyFullSyncs_,
                                                   kEmptyFullSyncWipeThreshold, EmptyStreakElapsed(),
                                                   EmptyFullSyncMinStreakElapsed())) {
            // Rows a sibling pane is displaying are not stale — this session's query simply does
            // not cover them (see FilterStaleIdsRetainedElsewhere). Same subtraction as the
            // streaming path, applied here as a set membership test to keep the single pass.
            const std::vector<std::string> retainedElsewhereVec = deps_.TicketIdsRetainedByOtherContexts();
            const std::unordered_set<std::string> retainedElsewhere(retainedElsewhereVec.begin(),
                                                                    retainedElsewhereVec.end());
            std::size_t retainedSkipped = 0;
            for (const auto& row : existing) {
                if (keepIds.find(row.id) != keepIds.end()) {
                    continue;
                }
                if (retainedElsewhere.find(row.id) != retainedElsewhere.end()) {
                    ++retainedSkipped;
                    continue;
                }
                deps_.Cache()->DeleteTicket(backendKey, row.id);
                ++deleted;
            }
            if (retainedSkipped > 0) {
                LOG_INFO("TicketSyncService::ApplyIssueFetchPack kept %zu stale-marked row(s) retained by other live "
                         "pane(s).",
                         retainedSkipped);
            }
        }
    }

    LOG_INFO("TicketSyncService::ApplyIssueFetchPack finished fetched=%zu saved=%zu deleted=%zu fullSync=%d",
             freshTickets.size(), saved, deleted, fullSyncCompleted ? 1 : 0);

    // Coalesce Lua window dirty-bumps to once-per-session: many ApplyIssueFetchPack calls
    // can run during a single streaming fetch. NotifyLuaTicketDataChanged fires once at
    // session end (TickStreamingApply finalisation). Invariant: every ActiveTickets-
    // mutating path flips this flag in the same scope.
    deps_.SetPendingLuaWindowBump(true);
}

void TicketSyncService::TickStreamingApply() {
    if (startsBlocked_.load()) {
        return; // the cache is being recreated: nothing may start, and nothing may touch it
    }
    // 1. Drain a superseded session, else start a pending request once the previous session
    //    (worker + apply queue + stale cleanup) is fully drained.
    const bool isWorkerActive = activeStreamingSync_.Active.load();
    const bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load() ||
                               deleteInFlight_->load();

    if (activeStreamingSync_.Superseded.load()) {
        if (DiscardSupersededSessionIfNeeded()) {
            return;
        }
    }

    if (!isSessionBusy) {
        if (StartPendingSyncIfIdle(isSessionBusy)) {
            return;
        }
    }

    // 2. Progressive, budgeted stale ticket deletion over multiple frames to avoid UI hitches.
    if (isDeletingStale_.load()) {
        if (DrainStaleDeletionBudget()) {
            return;
        }
    }

    if (!activeStreamingSync_.ActiveSessionRunning) {
        return;
    }

    // 3. Process incoming streaming ticket batches in-memory and write to SQLite with budget.
    DrainPendingStreamingBatches();

    // 4. When the worker finished and the queue is drained, finalise the session.
    FinalizeStreamingSessionIfDone();
}

bool TicketSyncService::DiscardSupersededSessionIfNeeded() {
    const bool isWorkerActive = activeStreamingSync_.Active.load();
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();
    }
    staleIdsToDelete_.clear();
    isDeletingStale_.store(false);
    totalStaleToDelete_ = 0;
    staleDeletedSoFar_ = 0;

    if (isWorkerActive) {
        return true;
    }
    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }
    activeStreamingSync_.Active = false;
    activeStreamingSync_.ActiveSessionRunning = false;
    activeStreamingSync_.FullSyncCompleted = false;
    activeStreamingSync_.TotalFetchedCount = 0;
    {
        // FetchError contract: every read/write through QueueMutex (worker joined above).
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.FetchErrorTransient = false;
        activeStreamingSync_.Warning.clear();
    }
    activeStreamingSync_.KeepIds.clear();
    activeStreamingSync_.Cancelled = false;
    activeStreamingSync_.Superseded = false;

    LOG_INFO("TicketSyncService::TickStreamingApply discarded superseded streaming sync session.");

    // Same gate as the normal path (TickStreamingApply): a stale-row DELETE still in flight would race the
    // new session's saves, so the request stays pending until StartPendingSyncIfIdle sees the service idle.
    if (hasPendingSyncRequest_ && !deleteInFlight_->load()) {
        hasPendingSyncRequest_ = false;
        StartStreamingSync(pendingConfig_, pendingViews_, pendingMode_);
        pendingMode_ = smatchet::sync::SyncMode::Delta;
    }
    return true;
}

bool TicketSyncService::StartPendingSyncIfIdle(bool /*isSessionBusy*/) {
    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }
    if (hasPendingSyncRequest_) {
        hasPendingSyncRequest_ = false;
        StartStreamingSync(pendingConfig_, pendingViews_, pendingMode_);
        pendingMode_ = smatchet::sync::SyncMode::Delta;
        return true;
    }
    return false;
}

bool TicketSyncService::DrainStaleDeletionBudget() {
    // The in-memory roster drops every stale row at once (one pass, one republish); the SQLite rows go in ONE
    // worker-side transaction. Nothing here touches SQLite on the UI thread.
    std::vector<std::string> ids;
    ids.swap(staleIdsToDelete_);
    if (!ids.empty()) {
        const std::unordered_set<std::string> stale(ids.begin(), ids.end());
        {
            std::lock_guard<std::mutex> lock(deps_.ActiveTicketsMutex());
            std::vector<CachedTicket>& active = deps_.ActiveTickets();
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&stale](const CachedTicket& t) { return stale.find(t.id) != stale.end(); }),
                         active.end());
            deps_.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>(active));
            deps_.BumpActiveTicketsRevision();
        }
        // Stale-deletion shrinks visible state - windows that show ticket lists / counts must re-record.
        deps_.SetPendingLuaWindowBump(true);

        const std::shared_ptr<ISyncCache> cache = deps_.CacheShared();
        if (cache) {
            const std::string backendKey = deps_.CacheBackendKey();
            const std::shared_ptr<std::atomic<bool>> inFlight = deleteInFlight_;
            inFlight->store(true);
            try {
                deps_.LaunchBackgroundTask([cache, backendKey, ids, inFlight]() {
                    /* PILLAR2_WORKER_ONLY */ // est-latency: one batched delete transaction - off the UI thread
                    // Clears the busy flag on every exit, including a non-std exception: a flag left set would
                    // block every later sync for good.
                    struct ClearOnExit {
                        std::atomic<bool>& Flag;
                        ~ClearOnExit() { Flag.store(false); }
                    } clearOnExit{*inFlight};
                    try {
                        cache->DeleteTickets(backendKey, ids);
                    } catch (const std::exception& ex) {
                        LOG_ERROR("TicketSyncService: stale-row delete of %zu row(s) failed: %s", ids.size(),
                                  ex.what());
                    } catch (...) {
                        LOG_ERROR("TicketSyncService: stale-row delete of %zu row(s) threw a non-std exception.",
                                  ids.size());
                    }
                });
            } catch (...) {
                inFlight->store(false); // the task never started
                LOG_ERROR("TicketSyncService: stale-row delete of %zu row(s) could not be started.", ids.size());
            }
        }
    }
    staleDeletedSoFar_ = totalStaleToDelete_;
    isDeletingStale_.store(false);
    LOG_INFO("TicketSyncService::TickStreamingApply finished stale deletion. total_deleted=%zu", totalStaleToDelete_);
    // Trigger editmeta warmup after cleanup completes
    deps_.WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
    // Flush coalesced Lua window bump now that stale deletion finished. The session-end flush can't see this
    // because stale deletion runs after the session-end block has already fired.
    if (deps_.GetPendingLuaWindowBump()) {
        deps_.NotifyLuaTicketDataChanged();
        deps_.SetPendingLuaWindowBump(false);
    }
    return true;
}

void TicketSyncService::DrainPendingStreamingBatches() {
    auto start = std::chrono::high_resolution_clock::now();
    std::size_t fetchedInFrame = 0;
    std::size_t unchangedInFrame = 0;
    std::vector<CachedTicket> batchToProcess;
    std::vector<CachedTicket> fetchedThisFrame;
    std::vector<CachedTicket> unchangedThisFrame;

    while (true) {
        batchToProcess.clear();
        bool cachedUnchanged = false;
        {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            if (activeStreamingSync_.PendingBatches.empty()) {
                break;
            }
            SyncBatch& frontBatch = activeStreamingSync_.PendingBatches.front();
            for (const std::string& keepId : frontBatch.KeepOnlyIds) {
                activeStreamingSync_.KeepIds.insert(keepId);
            }
            frontBatch.KeepOnlyIds.clear();
            if (frontBatch.Rows.empty()) {
                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());
                continue;
            }
            cachedUnchanged = frontBatch.CachedUnchanged;
            const std::size_t used = cachedUnchanged ? unchangedInFrame : fetchedInFrame;
            const std::size_t limit = cachedUnchanged ? kUnchangedRowsPerFrame : kFetchedRowsPerFrame;
            const std::size_t sliceSize = (std::min)(frontBatch.Rows.size(), limit - used);
            if (sliceSize == 0) {
                break; // Frame limit
            }

            const std::ptrdiff_t sliceOffset = static_cast<std::ptrdiff_t>(sliceSize);
            batchToProcess.insert(batchToProcess.end(), std::make_move_iterator(frontBatch.Rows.begin()),
                                  std::make_move_iterator(frontBatch.Rows.begin() + sliceOffset));
            frontBatch.Rows.erase(frontBatch.Rows.begin(), frontBatch.Rows.begin() + sliceOffset);

            if (frontBatch.Rows.empty()) {
                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());
            }
        }

        // The worker already persisted the batch (QueueBatch), so this thread touches memory only. Both kinds
        // keep a lazily fetched comment thread the resident row gained since the worker prepared the batch.
        CarryForwardLazyComments(deps_, batchToProcess);
        for (const auto& t : batchToProcess) {
            if (!t.id.empty()) {
                activeStreamingSync_.KeepIds.insert(t.id);
            }
        }
        std::size_t& counter = cachedUnchanged ? unchangedInFrame : fetchedInFrame;
        counter += batchToProcess.size();
        std::vector<CachedTicket>& frameRows = cachedUnchanged ? unchangedThisFrame : fetchedThisFrame;
        frameRows.insert(frameRows.end(), std::make_move_iterator(batchToProcess.begin()),
                         std::make_move_iterator(batchToProcess.end()));

        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        const std::size_t limit = cachedUnchanged ? kUnchangedRowsPerFrame : kFetchedRowsPerFrame;
        if (elapsed >= 3 || counter >= limit) {
            break;
        }
    }

    if (fetchedThisFrame.empty() && unchangedThisFrame.empty()) {
        return;
    }
    std::vector<std::string> movedIds;
    const bool changed = MergeRowsIntoActiveTickets(deps_, mergeIndex_, fetchedThisFrame, unchangedThisFrame, movedIds);
    if (!movedIds.empty()) {
        deps_.OnTicketsRemotelyChanged(movedIds);
    }
    if (changed || !fetchedThisFrame.empty()) {
        deps_.PruneEditMetaCacheToActiveTickets();
        // Streaming-batch end-of-batch - flip the coalesced bump; session-end emits once.
        deps_.SetPendingLuaWindowBump(true);
    }
}

void TicketSyncService::FinalizeStreamingSessionIfDone() {
    bool isWorkerFinished = !activeStreamingSync_.Active.load();
    bool hasPending = false;
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        hasPending = !activeStreamingSync_.PendingBatches.empty();
    }

    if (!(isWorkerFinished && !hasPending && activeStreamingSync_.ActiveSessionRunning)) {
        return;
    }
    activeStreamingSync_.ActiveSessionRunning = false;

    // FetchError plus Warning are written by the worker thread under QueueMutex; acquire that
    // mutex while reading them. The full-sync-completed flag is atomic and needs no lock.
    std::string fetchError;
    bool fetchErrorTransient = false;
    std::string fetchWarning;
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        fetchError = activeStreamingSync_.FetchError;
        fetchErrorTransient = activeStreamingSync_.FetchErrorTransient;
        fetchWarning = activeStreamingSync_.Warning;
    }

    bool fullSyncCompleted = activeStreamingSync_.FullSyncCompleted;

    // Classification travels with the error from the worker-side seam (N12 slice 1).
    if (!fetchError.empty() && fetchErrorTransient) {
        deps_.SetLastTrackerTicketSyncWarning("Showing cached issues — live refresh did not complete: " + fetchError,
                                              /*transient=*/true);
        LOG_WARN("TicketSyncService::TickStreamingApply transport-style fetch issue: %s", fetchError.c_str());
        deps_.SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState::TransportDown);
        const auto nowProbe = std::chrono::steady_clock::now();
        deps_.SetNextTrackerConnectivityProbeAt(nowProbe);
        deps_.PushOfflineReplayTimersDuringTransportOutage(nowProbe);
        deps_.NotifySyncStatus("Sync Failed", fetchError, ITicketSyncDeps::SyncNotifyLevel::Error, 5000);
    } else if (!fetchError.empty()) {
        deps_.NotifySyncStatus("Sync Warning", fetchError, ITicketSyncDeps::SyncNotifyLevel::Warning, 5000);
    } else {
        // Soft warnings: data is good, just partial — still notify success but surface
        // the caveat as a warning banner + toast.
        if (!fetchWarning.empty()) {
            deps_.SetLastTrackerTicketSyncWarning("Sync completed with a caveat: " + fetchWarning,
                                                  /*transient=*/false);
            LOG_WARN("TicketSyncService::TickStreamingApply soft warning: %s", fetchWarning.c_str());
            deps_.NotifySyncStatus("Sync Warning", fetchWarning, ITicketSyncDeps::SyncNotifyLevel::Warning, 5000);
        }
        deps_.RequestDeferredLiveTrackerBackendSuccessNotify();

        std::string msg =
            "Synchronized " + std::to_string(activeStreamingSync_.KeepIds.size()) + " issues successfully.";
        deps_.NotifySyncStatus("Sync Complete", msg, ITicketSyncDeps::SyncNotifyLevel::Success, 4000);
    }

    // Session-end deps hook (review MEDIUM-1): a failed session re-arms the owning pane
    // context's initial-sync latch so the next focus switch retries instead of adopting
    // the failed result forever. UI thread (TickStreamingApply), once per session.
    deps_.OnStreamingSyncSessionFinished(fetchError.empty());

    totalStaleToDelete_ = 0;
    staleDeletedSoFar_ = 0;
    staleIdsToDelete_.clear();

    // Track consecutive zero-keep full syncs (UI thread only), mirroring ApplyIssueFetchPack.
    // A full sync that kept at least one ticket resets the streak; a partial fetch leaves it
    // untouched. The streak feeds the empty-full-sync wipe guard below.
    const std::size_t keptThisSession = activeStreamingSync_.KeepIds.size();
    NoteFullSyncEmptiness(fullSyncCompleted, keptThisSession == 0);

    mergeIndex_.Release();
    RecordSessionStats(fetchError, fullSyncCompleted);
    SeedStaleDeletionForSession(fullSyncCompleted, keptThisSession);
    ConvergeActiveTicketsToSessionKeepSet(fullSyncCompleted, keptThisSession);
    PublishViewListing(fullSyncCompleted);

    LOG_INFO("TicketSyncService::TickStreamingApply finished sync session. saved_or_kept=%zu total_stale=%zu "
             "fullSync=%d err=%s",
             activeStreamingSync_.KeepIds.size(), totalStaleToDelete_, fullSyncCompleted ? 1 : 0, fetchError.c_str());

    if (!isDeletingStale_.load()) {
        deps_.WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
    }

    // Session-end Lua window bump: emit exactly once if any apply-pack, stale-deletion,
    // or streaming-batch scope set the flag during this session. Skipping when no flag
    // was set avoids needless re-records when nothing actually changed.
    if (deps_.GetPendingLuaWindowBump()) {
        deps_.NotifyLuaTicketDataChanged();
        deps_.SetPendingLuaWindowBump(false);
    }
}

smatchet::sync::SyncSessionStats TicketSyncService::LastSessionStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return lastStats_;
}

void TicketSyncService::RecordSessionStats(const std::string& fetchError, bool fullSyncCompleted) {
    smatchet::sync::SyncSessionStats stats;
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        stats = activeStreamingSync_.WorkerStats;
    }
    const smatchet::sync::TrackerTrafficSample end = deps_.SampleTrackerTraffic();
    const smatchet::sync::TrackerTrafficSample& start = activeStreamingSync_.TrafficAtStart;
    stats.Valid = true;
    if (stats.RequestId == 0) {
        stats.RequestId = activeStreamingSync_.RequestId.load();
    }
    std::string recordedError = fetchError;
    if (stats.Mode.empty()) {
        stats.Mode = "full"; // the worker never reached its result seam (exception / early return)
        if (recordedError.empty()) {
            recordedError = "cancelled or superseded before a result";
        }
    }
    stats.Requests = end.Requests >= start.Requests ? end.Requests - start.Requests : 0;
    stats.UploadBytes = end.UploadBytes >= start.UploadBytes ? end.UploadBytes - start.UploadBytes : 0;
    stats.DownloadBytes = end.DownloadBytes >= start.DownloadBytes ? end.DownloadBytes - start.DownloadBytes : 0;
    stats.DurationMs =
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - activeStreamingSync_.SessionStartedAt)
                                   .count());
    stats.Completed = fullSyncCompleted;
    stats.Error = recordedError;
    LOG_INFO("TicketSyncService: sync stats req=%llu mode=%s roster=%zu bodies=%zu unchanged=%zu requests=%llu "
             "down=%llu up=%llu durationMs=%lld completed=%d fallback='%s' err='%s'",
             static_cast<unsigned long long>(stats.RequestId), stats.Mode.c_str(), stats.RosterSize,
             stats.BodiesFetched, stats.Unchanged, static_cast<unsigned long long>(stats.Requests),
             static_cast<unsigned long long>(stats.DownloadBytes), static_cast<unsigned long long>(stats.UploadBytes),
             stats.DurationMs, stats.Completed ? 1 : 0, stats.FallbackReason.c_str(), stats.Error.c_str());
    std::lock_guard<std::mutex> lock(statsMutex_);
    lastStats_ = std::move(stats);
}

void TicketSyncService::SeedStaleDeletionForSession(bool fullSyncCompleted, std::size_t keptThisSession) {
    if (!fullSyncCompleted) {
        return;
    }

    // Ids sibling panes are displaying, collected BEFORE QueueMutex is taken: the collector locks
    // gridContextsMutex_ then a per-context activeTicketsMutex_, and DrainPendingStreamingBatches
    // already takes activeTicketsMutex_ -> QueueMutex, so collecting under QueueMutex would invert
    // that order.
    const std::vector<std::string> retainedElsewhere = deps_.TicketIdsRetainedByOtherContexts();

    std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
    // Empty-full-sync guard (finding DR4, hardened for #2143): a completed full sync that kept
    // zero tickets marks every cached row stale, which would wipe the entire offline cache. Treat
    // a zero-keep full sync as suspect and hold off the mass deletion until the empty result has
    // repeated kEmptyFullSyncWipeThreshold times AND held for EmptyFullSyncMinStreakElapsed() —
    // the count alone was satisfiable by two flaps seconds apart (#2143).
    const std::chrono::milliseconds streakElapsed = EmptyStreakElapsed();
    const bool skipWipe = ShouldSkipMassDeletionOnEmptyFullSync(
        keptThisSession, activeStreamingSync_.BackgroundStaleIds.size(), consecutiveEmptyFullSyncs_,
        kEmptyFullSyncWipeThreshold, streakElapsed, EmptyFullSyncMinStreakElapsed());
    if (skipWipe) {
        LOG_WARN("TicketSyncService::TickStreamingApply skipped mass stale deletion on suspect empty full sync "
                 "(cached=%zu, consecutive_empty=%d, streak_held=%llds, required=%llds).",
                 activeStreamingSync_.BackgroundStaleIds.size(), consecutiveEmptyFullSyncs_,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(streakElapsed).count()),
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::seconds>(EmptyFullSyncMinStreakElapsed()).count()));
        activeStreamingSync_.BackgroundStaleIds.clear();
        return;
    }

    // Sibling-pane subtraction: a row another live pane is displaying is outside THIS query's
    // scope, not deleted on the remote (see FilterStaleIdsRetainedElsewhere).
    const std::size_t markedStale = activeStreamingSync_.BackgroundStaleIds.size();
    staleIdsToDelete_ = FilterStaleIdsRetainedElsewhere(activeStreamingSync_.BackgroundStaleIds, retainedElsewhere);
    activeStreamingSync_.BackgroundStaleIds.clear();
    if (staleIdsToDelete_.size() < markedStale) {
        LOG_INFO("TicketSyncService::TickStreamingApply kept %zu of %zu stale-marked row(s) retained by other "
                 "live pane(s).",
                 markedStale - staleIdsToDelete_.size(), markedStale);
    }

    if (!staleIdsToDelete_.empty()) {
        isDeletingStale_.store(true);
        totalStaleToDelete_ = staleIdsToDelete_.size();
        staleDeletedSoFar_ = 0;
    }
}

void TicketSyncService::ConvergeActiveTicketsToSessionKeepSet(bool fullSyncCompleted, std::size_t keptThisSession) {
    // Only a completed full sync knows the whole answer for this pane's query, and only a
    // non-empty one is trusted (a zero-keep full sync is the suspect case SeedStaleDeletionForSession
    // refuses to act on — pruning here would empty the grid on the same transient glitch).
    if (!fullSyncCompleted || keptThisSession == 0) {
        return;
    }
    // KeepIds is read without QueueMutex on purpose: the streaming worker has already exited by the
    // time TickStreamingApply finalizes a session (same lock-free read as the LOG_INFO above).
    const std::unordered_set<std::string>& keep = activeStreamingSync_.KeepIds;

    // The shared backend-keyed cache spans every pane (ADR-0018 decision 4), so a batch apply can
    // have admitted rows belonging to a sibling pane's query. Converge this pane's in-memory roster
    // onto exactly what its OWN query returned, republishing the immutable snapshot and bumping the
    // revision once under one lock (same contract as DrainStaleDeletionBudget) so readers never see
    // a half-pruned roster.
    std::vector<std::string> ownedIds;
    {
        std::lock_guard<std::mutex> lock(deps_.ActiveTicketsMutex());
        std::vector<CachedTicket>& rows = deps_.ActiveTickets();
        const std::size_t dropped = smatchet::RetainTicketsInKeepSet(rows, keep);
        ownedIds.reserve(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            ownedIds.push_back(rows[i].id);
        }
        if (dropped != 0) {
            deps_.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>(rows));
            deps_.BumpActiveTicketsRevision();
            LOG_INFO("TicketSyncService::TickStreamingApply converged pane roster to its own query (dropped %zu, "
                     "%zu remain).",
                     dropped, rows.size());
        }
    }
    // Published OUTSIDE the roster lock: the adapter reads the context's backend key under its own
    // mutex, and activeTicketsMutex_ must stay the innermost of that pair.
    deps_.PublishOwnedTicketIds(ownedIds);
}

void TicketSyncService::PublishViewListing(bool fullSyncCompleted) {
    std::vector<std::string> listed;
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        listed.swap(activeStreamingSync_.ViewListedIds);
    }
    if (fullSyncCompleted) {
        deps_.PublishViewListedIds(listed);
    }
}

bool TicketSyncService::IsActive() const {
    return activeStreamingSync_.Active.load() || activeStreamingSync_.ActiveSessionRunning.load() ||
           isDeletingStale_.load() || deleteInFlight_->load();
}

bool TicketSyncService::HasPendingWork() const { return IsActive() || viewsLoadsInFlight_->load() > 0; }

void TicketSyncService::ResetStaleDeletionState() {
    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();
        activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.FetchErrorTransient = false;
        activeStreamingSync_.Warning.clear();
    }
    staleIdsToDelete_.clear();
    isDeletingStale_.store(false);
    totalStaleToDelete_ = 0;
    staleDeletedSoFar_ = 0;
    activeStreamingSync_.FullSyncCompleted = false;
    activeStreamingSync_.TotalFetchedCount = 0;
    activeStreamingSync_.KeepIds.clear();
    activeStreamingSync_.Cancelled = false;
    activeStreamingSync_.Superseded = false;
}

void TicketSyncService::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                                        smatchet::sync::SyncMode mode) {
    if (startsBlocked_.load()) {
        LOG_INFO("TicketSyncService::SyncWithBackend held back: the local cache is being recreated; it runs when the "
                 "recreate finishes.");
        RecordBlockedSyncRequest_(configOverride, viewsOverride, mode);
        return;
    }
    if (hasFoldedMode_) {
        mode = smatchet::sync::CombineSyncModes(foldedMode_, mode); // the held request rides on this one
        hasFoldedMode_ = false;
    }
    LOG_INFO("TicketSyncService::SyncWithBackend started (asynchronous streaming refresh).");

    if (configOverride == nullptr || viewsOverride == nullptr) {
        // The config file and the views file are read on a worker (Pillar 2): a recreate's follow-up and a
        // Preferences save pass no config, and parsing it here would run on the frame.
        SyncWithBackendLoadingViews_(configOverride, viewsOverride, mode);
        return;
    }
    TrackerConfig cfgCopy = *configOverride;
    ViewsStore viewsCopy = *viewsOverride;

    const bool isWorkerActive = activeStreamingSync_.Active.load();
    const bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load() ||
                               deleteInFlight_->load();

    if (isSessionBusy) {
        activeStreamingSync_.Cancelled = true;
        activeStreamingSync_.Superseded = true;
        // Full is sticky: neither a queued Full nor a Full whose fetch is still running is downgraded by a later
        // ordinary refresh (once the fetch is done, the session only drains and a Delta is enough).
        const smatchet::sync::SyncMode held = hasPendingSyncRequest_ ? pendingMode_
                                              : isWorkerActive       ? activeMode_
                                                                     : smatchet::sync::SyncMode::Delta;
        pendingMode_ = smatchet::sync::CombineSyncModes(held, mode);
        hasPendingSyncRequest_ = true;
        pendingConfig_ = cfgCopy;
        pendingViews_ = viewsCopy;
        LOG_INFO("TicketSyncService: Active sync/apply/cleanup session busy. New sync request "
                 "deferred to avoid UI thread block.");
        return;
    }

    StartStreamingSync(cfgCopy, viewsCopy, mode);
}

void TicketSyncService::SyncWithBackendLoadingViews_(const TrackerConfig* configOverride,
                                                     const ViewsStore* viewsOverride, smatchet::sync::SyncMode mode) {
    ITicketSyncDeps* const deps = &deps_;
    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    const std::shared_ptr<std::atomic<int>> inFlight = viewsLoadsInFlight_;
    TicketSyncService* const self = this;
    // Whatever the caller supplied is copied: the worker outlives this call. A part left null is read on the worker.
    const bool haveCfg = configOverride != nullptr;
    const bool haveViews = viewsOverride != nullptr;
    TrackerConfig cfgGiven;
    if (haveCfg) {
        cfgGiven = *configOverride;
    }
    ViewsStore viewsGiven;
    if (haveViews) {
        viewsGiven = *viewsOverride;
    }
    // Counted while it runs: the pane does not retire (HasPendingWork), so `deps`, which the pane's context owns,
    // outlives this task. Raised before the launch, lowered by the task itself (or here when it never starts).
    inFlight->fetch_add(1);
    try {
        deps_.LaunchBackgroundTask([self, deps, alive, inFlight, haveCfg, haveViews, cfgGiven, viewsGiven, mode]() {
            /* PILLAR2_WORKER_ONLY */ // est-latency: 5ms - config + views file read and parse, off the UI thread
            // A missing config file loads as the defaults, the same as sync.full.
            const TrackerConfig cfg = haveCfg ? cfgGiven : ConfigManager::Load();
            ViewsStore views = haveViews ? viewsGiven : ConfigManager::LoadViewsOrBootstrap(cfg);
            if (alive->load()) { // a destroyed service has nothing to sync, and its deps may be gone
                deps->PostToMainThread([self, alive, cfg, views, mode]() {
                    if (!alive->load()) {
                        return; // the pane's service was destroyed while the files were read
                    }
                    self->SyncWithBackend(&cfg, &views, mode);
                });
            }
            inFlight->fetch_sub(1);
        });
    } catch (...) {
        inFlight->fetch_sub(1); // the task never started
        throw;
    }
}

void TicketSyncService::SwapBackendIfTrackerChanged(const TrackerConfig& cfgCopy) {
    // Swap Backend type safely before starting the worker.
    std::string newTracker = cfgCopy.TrackerType;
    if (newTracker.empty())
        newTracker = "Jira";
    const std::string trackerLower = ToLowerAsciiCopy(newTracker);
    const std::string currentType = deps_.BackendConnectivity() ? deps_.BackendConnectivity()->GetTrackerType() : "";
    // Normalize currentType to lowercase too — JiraClient/PlaneClient report mixed case
    // ("Jira" / "Plane") while GitHubClient reports lowercase ("github"). Compare in one
    // case to avoid silent miss on a GitHub→Plane switch (etc.).
    const std::string currentLower = ToLowerAsciiCopy(currentType);
    const bool isCurrentlyJira = (currentLower == "jira");
    const bool isCurrentlyPlane = (currentLower == "plane");
    const bool isCurrentlyGitHub = (currentLower == "github");
    const bool isCurrentlyLinear = (currentLower == "linear");

    // Issue #979: pass the live cfgCopy into every Create — the factory must build the
    // new client from the caller's in-memory config, not a disk re-read that races the
    // debounced prefs save.
    const std::string newCacheKey = smatchet::cache_keys::TrackerCacheBackendKey(cfgCopy);
    bool backendSwapped = false;
    if (trackerLower == "plane" && !isCurrentlyPlane) {
        deps_.SetBackend(deps_.BackendFactory()->Create("Plane", cfgCopy));
        LOG_INFO("TicketSyncService: Switched backend to Plane.");
        backendSwapped = true;
    } else if (trackerLower == "jira" && !isCurrentlyJira) {
        deps_.SetBackend(deps_.BackendFactory()->Create("Jira", cfgCopy));
        LOG_INFO("TicketSyncService: Switched backend to Jira.");
        backendSwapped = true;
    } else if (trackerLower == "github" && !isCurrentlyGitHub) {
        deps_.SetBackend(deps_.BackendFactory()->Create("GitHub", cfgCopy));
        LOG_INFO("TicketSyncService: Switched backend to GitHub.");
        backendSwapped = true;
    } else if (trackerLower == "linear" && !isCurrentlyLinear) {
        deps_.SetBackend(deps_.BackendFactory()->Create("Linear", cfgCopy));
        LOG_INFO("TicketSyncService: Switched backend to Linear.");
        backendSwapped = true;
    } else if (!lastAppliedCacheKey_.empty() && newCacheKey != lastAppliedCacheKey_) {
        // Same tracker kind, another site or account (#2268): a Jira host or account, a Plane
        // workspace, a GitHub repo, a Linear team. Build the client fresh (it may hold per-site
        // state such as Plane's key-to-id map) and treat it as a backend switch below.
        const std::string kind = ConfigManager::NormalizeViewsBackendKey(newTracker);
        deps_.SetBackend(deps_.BackendFactory()->Create(kind, cfgCopy));
        LOG_INFO("TicketSyncService: Recreated %s client for site '%s'.", kind.c_str(),
                 smatchet::cache_keys::DescribeCacheBackendKey(newCacheKey).c_str());
        backendSwapped = true;
    }
    lastAppliedCacheKey_ = newCacheKey;

    // Re-stamp the cache namespace to match the requested tracker site (multi-grid Slice 1b,
    // #2268). Unconditional + idempotent: also covers a context whose key was never wired (e.g.
    // a test fixture) so the first sync writes under the right namespace, not under "".
    deps_.SetCacheBackendKey(newCacheKey);

    // Backend switch (another kind, or another site of the same kind): clear in-memory tickets
    // so the old backend's items don't linger in the grid while the new backend's first fetch
    // is in flight. Without this, switching Jira → GitHub (or site A → site B) leaves stale
    // tickets visible and the per-row update path tries to mutate them against the new
    // backend, which fails or silently writes to the wrong tracker. SQLite cache rows survive
    // under their own key — switching back later re-populates ActiveTickets via the cache
    // hydrate path.
    if (backendSwapped) {
        refusedKeys_.Clear(); // another tracker or site: what the old one refused says nothing about this one
        {
            // Clear + publish under ONE ActiveTicketsMutex scope (issue #1081). Publishing
            // outside the lock raced worker-thread writers (offline-replay RefreshLocalData,
            // UpdateTicket) and locked readers (GetActiveTicketsSnapshot) on the shared_ptr
            // control block — C++14 UB, the primary std::terminate candidate of the crash.
            // Contract: GridLiveContext.h — activeTicketsPublished_ is guarded by
            // activeTicketsMutex_.
            std::lock_guard<std::mutex> lk(deps_.ActiveTicketsMutex());
            deps_.ActiveTickets().clear();
            deps_.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>());
        }
        deps_.BumpActiveTicketsRevision();
        // Invariant (see file-level comment ~L108): every ActiveTickets-mutating path
        // flips the Lua-window-bump flag so downstream Lua-side window state stays
        // synchronised with the grid.
        deps_.SetPendingLuaWindowBump(true);
        LOG_INFO("TicketSyncService: Cleared in-memory ActiveTickets on backend change.");
    }
}

void TicketSyncService::StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                           smatchet::sync::SyncMode mode) {
    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }

    deps_.NotifySyncStatus("Syncing", "Refreshing issues from Tracker...", ITicketSyncDeps::SyncNotifyLevel::Info,
                           2500);

    SwapBackendIfTrackerChanged(cfgCopy);

    const std::uint64_t reqId = ++currentFetchRequestId_;
    ++sessionEpoch_;
    activeMode_ = mode;
    mergeIndex_.Release(); // a new session builds its map once, from the roster as it is then
    if (mode == smatchet::sync::SyncMode::Full) {
        refusedKeys_.Clear(); // an explicit full sync asks again about every key (the worker is joined above)
    }
    activeStreamingSync_.SessionStartedAt = std::chrono::steady_clock::now();
    activeStreamingSync_.TrafficAtStart = deps_.SampleTrackerTraffic();
    activeStreamingSync_.RequestId = reqId;
    activeStreamingSync_.Cancelled = false;
    activeStreamingSync_.Superseded = false;
    activeStreamingSync_.Active = true;
    activeStreamingSync_.ActiveSessionRunning = true;
    activeStreamingSync_.TotalFetchedCount = 0;
    activeStreamingSync_.FullSyncCompleted = false;
    activeStreamingSync_.KeepIds.clear();
    {
        // FetchError contract: every read/write through QueueMutex. Worker for the new request
        // has not been spawned yet, so the lock is uncontended.
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.FetchErrorTransient = false;
        activeStreamingSync_.Warning.clear();
        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();
        activeStreamingSync_.ViewListedIds.clear();
        activeStreamingSync_.WorkerStats = smatchet::sync::SyncSessionStats();
    }

    LOG_INFO("TicketSyncService: Spawning background worker for async streaming fetch request ID=%llu",
             static_cast<unsigned long long>(reqId));

    activeStreamingSync_.WorkerThread = std::thread(
        [this, reqId, cfgCopy, viewsCopy, mode]() { RunStreamingWorkerBody(reqId, cfgCopy, viewsCopy, mode); });
}

std::string TicketSyncService::LoadSessionMeta() {
    sessionMeta_.clear();
    if (!deps_.Cache()) {
        return std::string();
    }
    try {
        sessionMeta_ = deps_.Cache()->GetTicketSyncMeta(deps_.CacheBackendKey());
    } catch (const std::exception& ex) {
        // The snapshot only guards against rolling a row back; a sync must not fail over it.
        LOG_WARN("TicketSyncService: could not read cached ticket versions: %s", ex.what());
        sessionMeta_.clear();
        return ex.what();
    } catch (...) {
        LOG_WARN("TicketSyncService: could not read cached ticket versions (unknown exception)");
        sessionMeta_.clear();
        return "unknown exception";
    }
    return std::string();
}

void TicketSyncService::RunStreamingWorkerBody(std::uint64_t reqId, const TrackerConfig& cfgCopy,
                                               const ViewsStore& viewsCopy, smatchet::sync::SyncMode mode) {
    try {
        // Versions of the rows already cached, so a fetched row older than its cached copy is never persisted.
        const std::string metaReadError = LoadSessionMeta();
        std::string fallbackReason;
        if (mode == smatchet::sync::SyncMode::Delta) {
            DeltaOutcome outcome;
            if (!metaReadError.empty()) {
                // An unreadable version table is not the expected "no cached ticket versions yet": say so.
                outcome.FallbackReason = "could not read cached ticket versions: " + metaReadError;
            } else {
                try {
                    outcome = RunDeltaFetch(reqId, cfgCopy, viewsCopy);
                } catch (const std::exception& ex) {
                    // Any failure on the delta path falls back to today's full fetch.
                    outcome.Finished = false;
                    outcome.FallbackReason = std::string("delta attempt threw: ") + ex.what();
                }
            }
            if (outcome.Finished) {
                sessionMeta_.clear(); // worker-only; released before the UI thread may start the next session
                activeStreamingSync_.Active = false;
                return;
            }
            fallbackReason = outcome.FallbackReason;
            if (outcome.ExpectedFallback) {
                LOG_DEBUG("TicketSyncService: full fetch instead of delta (%s) for request ID=%llu",
                          fallbackReason.c_str(), static_cast<unsigned long long>(reqId));
            } else {
                LOG_INFO("TicketSyncService: delta reload abandoned (%s); running a full fetch for request ID=%llu",
                         fallbackReason.c_str(), static_cast<unsigned long long>(reqId));
            }
        }
        RunFullFetch(reqId, cfgCopy, viewsCopy, fallbackReason);
        sessionMeta_.clear();
    } catch (const std::exception& ex) {
        sessionMeta_.clear();
        LOG_ERROR("TicketSyncService: Worker thread caught exception: %s", ex.what());
        if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FetchError = std::string("Sync failed with exception: ") + ex.what();
            // Deliberately non-transient (13a precedent): transport failures surface as classified
            // statuses from the backends, never as throws - a thrown worker is a bug shape, and
            // the non-transient branch is the safe ask-the-user path.
            activeStreamingSync_.FetchErrorTransient = false;
            activeStreamingSync_.FullSyncCompleted = false;
        }
    } catch (...) {
        sessionMeta_.clear();
        LOG_ERROR("TicketSyncService: Worker thread caught unknown exception.");
        if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FetchError = "Sync failed with unknown exception.";
            activeStreamingSync_.FetchErrorTransient = false; // fixed text - never transport-shaped
            activeStreamingSync_.FullSyncCompleted = false;
        }
    }

    activeStreamingSync_.Active = false;
}

void TicketSyncService::RunFullFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                     const std::string& fallbackReason) {
    std::unordered_set<std::string> workerKeepIds;
    std::vector<std::string> workerParentRefs;  // parent keys referenced by streamed rows (may repeat)
    std::vector<std::string> workerStreamedIds; // ids the view's own JQL/filter actually matched
    auto onBatch = [this, reqId, &workerKeepIds, &workerParentRefs,
                    &workerStreamedIds](std::vector<CachedTicket>&& batch) {
        for (const auto& ticket : batch) {
            if (!ticket.id.empty()) {
                workerKeepIds.insert(ticket.id);
                workerStreamedIds.push_back(ticket.id);
            }
            std::string parentKey = ParentHierarchyPure::ParentKeyOf(ticket);
            if (!parentKey.empty()) {
                workerParentRefs.push_back(std::move(parentKey));
            }
        }
        (void)QueueBatch(reqId, std::move(batch), /*cachedUnchanged=*/false);
    };
    auto shouldCancel = [this, reqId]() -> bool {
        return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
    };

    TrackerIssueFetchSummary summary =
        deps_.Backend()->FetchIssuesStreamed(onBatch, shouldCancel, &cfgCopy, &viewsCopy);

    if (summary.FetchError.empty() && !shouldCancel()) {
        FetchMissingParentsIntoQueue(reqId, cfgCopy, viewsCopy, workerParentRefs, workerKeepIds, summary);
    }
    if (summary.FetchError.empty() && !shouldCancel()) {
        FetchChildrenIntoQueue(reqId, cfgCopy, viewsCopy, workerStreamedIds, workerKeepIds, summary);
    }

    smatchet::sync::SyncSessionStats partial;
    partial.Mode = "full";
    partial.FallbackReason = fallbackReason;
    PublishWorkerResult(reqId, summary, workerKeepIds, partial, std::move(workerStreamedIds));
}

void TicketSyncService::PublishWorkerResult(std::uint64_t reqId, const TrackerIssueFetchSummary& summary,
                                            const std::unordered_set<std::string>& keepIds,
                                            const smatchet::sync::SyncSessionStats& partialStats,
                                            std::vector<std::string> viewListedIds) {
    if (activeStreamingSync_.RequestId != reqId || activeStreamingSync_.Cancelled) {
        return;
    }
    std::vector<std::string> localStaleIds;
    if (summary.FullSyncCompleted && deps_.Cache()) {
        // The deps getter hands back a mutex-guarded key copy - safe from this worker thread; the key
        // cannot change mid-session (swaps happen in StartStreamingSync, which joins the previous worker).
        std::vector<std::string> existingIds = deps_.Cache()->GetAllTicketIds(deps_.CacheBackendKey());
        std::copy_if(existingIds.begin(), existingIds.end(), std::back_inserter(localStaleIds),
                     [&keepIds](const std::string& id) { return keepIds.find(id) == keepIds.end(); });
    }

    std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
    activeStreamingSync_.FullSyncCompleted = summary.FullSyncCompleted;
    activeStreamingSync_.FetchError = summary.FetchError;
    // Worker-side classification seam (N12): the kind the backend classified at its own error site
    // (summary.Error, item 12). Every backend fills it whenever FetchError is set; an unclassified error is
    // a bug shape and lands on the safe non-transient branch.
    activeStreamingSync_.FetchErrorTransient = !summary.FetchError.empty() && summary.Error.IsRetryable();
    activeStreamingSync_.Warning = summary.Warning;
    activeStreamingSync_.TotalFetchedCount = summary.FetchedCount;
    activeStreamingSync_.WorkerStats = partialStats;
    activeStreamingSync_.WorkerStats.RequestId = reqId;
    activeStreamingSync_.WorkerStats.BodiesFetched = summary.FetchedCount;
    activeStreamingSync_.ViewListedIds = std::move(viewListedIds);
    if (summary.FullSyncCompleted && deps_.Cache()) {
        activeStreamingSync_.BackgroundStaleIds = std::move(localStaleIds);
    }
}

bool TicketSyncService::QueueBatch(std::uint64_t reqId, std::vector<CachedTicket>&& rows, bool cachedUnchanged) {
    if (rows.empty()) {
        return true;
    }
    std::vector<std::string> keepOnlyIds;
    if (!cachedUnchanged) {
        if (activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId) {
            return false; // superseded: do not spend a transaction on a discarded session
        }
        // Worker thread, no lock held: carry the comment thread forward (takes ActiveTicketsMutex alone),
        // drop rows older than the cached version, then persist rows + versions in one transaction.
        CarryForwardLazyComments(deps_, rows);
        // A Full sync is authoritative: it neither drops nor refuses a row for being older than the cached copy.
        const bool fullSync = activeMode_ == smatchet::sync::SyncMode::Full;
        const std::size_t dropped = fullSync ? 0 : DropRowsOlderThanCached(rows, sessionMeta_, keepOnlyIds);
        if (dropped > 0) {
            LOG_INFO("TicketSyncService: kept %zu cached row(s) newer than the fetched copy.", dropped);
        }
        if (deps_.Cache() && !rows.empty()) {
            SaveRowsWithMeta(*deps_.Cache(), deps_.CacheBackendKey(), rows, fullSync);
        }
        // A fetch (the fallback full fetch included) served these bodies: a key remembered as refused is forgotten
        // when its body arrives at the version it was refused for or a newer one. Worker thread, like refusedKeys_.
        if (!refusedKeys_.Empty()) {
            for (const CachedTicket& row : rows) {
                (void)refusedKeys_.NoteServed(row.id, row.GetFieldValueRef("updated"));
            }
        }
        if (rows.empty() && keepOnlyIds.empty()) {
            return true;
        }
    }
    // Worker thread, no lock held: the newer cached copies of the rows the fetch lost, queued as cache-served rows.
    std::vector<CachedTicket> newerCached = ReadCachedCopies(deps_.Cache(), deps_.CacheBackendKey(), keepOnlyIds);
    std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
    if (activeStreamingSync_.RequestId != reqId || activeStreamingSync_.Cancelled) {
        return false;
    }
    SyncBatch queued;
    queued.Rows = std::move(rows);
    queued.CachedUnchanged = cachedUnchanged;
    queued.KeepOnlyIds = std::move(keepOnlyIds);
    activeStreamingSync_.PendingBatches.push_back(std::move(queued));
    if (!newerCached.empty()) {
        SyncBatch cached;
        cached.Rows = std::move(newerCached);
        cached.CachedUnchanged = true;
        activeStreamingSync_.PendingBatches.push_back(std::move(cached));
    }
    return true;
}

namespace {
/// Ceiling on ancestor hops chased within a single sync. Independent of
/// `ParentHierarchyPure::kMaxHierarchyDepth` (which bounds *display* depth over whatever is
/// already cached): this bounds *network round-trips*, so pathological backend data can't turn
/// one sync into unbounded sequential fetches (a cycle already ends early through the keep-set
/// dedup). Every sync restarts the walk from the streamed rows, so levels past the cap are not
/// loaded by any sync; hitting it is logged and marks the sync partial (see
/// MarkHierarchyWalkIncomplete), so ancestors cached earlier are kept rather than purged.
constexpr int kMaxParentFetchHops = 16;

/// Ceiling on descendant hops chased within a single sync (the downward mirror of
/// kMaxParentFetchHops). Bounds network round-trips for a pathologically wide/deep tree (a
/// view showing one Epic that fans out into hundreds of stories/tasks/subtasks). As for the
/// parents, descendants past the cap are not loaded by any sync; hitting it is logged and marks
/// the sync partial, so descendants cached earlier are kept rather than purged.
constexpr int kMaxChildFetchHops = 16;

/// A parent / children walk that stopped before it finished (a failed request or a hop cap) did not
/// see every ticket the view holds through the hierarchy. The sync must then not be authoritative for
/// the stale purge: rows cached by an earlier, complete walk are not in this sync's keep set, and
/// treating them as gone would delete cached data after a failed fetch (Quality Pillar 6). The rows
/// this sync did fetch still apply.
void MarkHierarchyWalkIncomplete(TrackerIssueFetchSummary& summary, const std::string& warning) {
    summary.FullSyncCompleted = false;
    summary.AppendWarning(warning);
    LOG_WARN("TicketSyncService: %s", warning.c_str());
}

using smatchet::sync::FetchKeysIsolatingRefusals;
using smatchet::sync::RefusalSplitBudget;

/// The tracker refused these keys one by one: a definitive answer that repeats on every sync, so the
/// walk stays authoritative (a refused ticket's cached row may be purged) and the keys are named.
void NoteRefusedKeys(TrackerIssueFetchSummary& summary, const std::vector<std::string>& refusedKeys, const char* what) {
    if (refusedKeys.empty()) {
        return;
    }
    std::string warning = std::to_string(refusedKeys.size()) + " " + what + " refused by the tracker:";
    for (std::size_t i = 0; i < refusedKeys.size() && i < 5; ++i) {
        warning += " " + refusedKeys[i];
    }
    if (refusedKeys.size() > 5) {
        warning += " ...";
    }
    summary.AppendWarning(warning);
    LOG_WARN("TicketSyncService: %s", warning.c_str());
}

/// True when `keys` names a ticket this sync has not loaded: the hop cap then cut the walk short.
bool AnyKeyNotLoaded(const std::vector<std::string>& keys, const std::unordered_set<std::string>& loaded) {
    for (const std::string& key : keys) {
        if (loaded.count(key) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace

void TicketSyncService::FetchMissingParentsIntoQueue(std::uint64_t reqId, const TrackerConfig& cfgCopy,
                                                     const ViewsStore& viewsCopy,
                                                     const std::vector<std::string>& parentRefs,
                                                     std::unordered_set<std::string>& workerKeepIds,
                                                     TrackerIssueFetchSummary& summary, const HierarchyDelta* delta) {
    if (parentRefs.empty()) {
        return;
    }
    // Global opt-out (Preferences -> Editing -> Grid behaviour -> Load parent issues).
    if (!cfgCopy.LoadParentIssues) {
        return;
    }
    // Per-view opt-out: a view that hides parents never pays for the keyed fetch (FS parity).
    const ViewDefinition* activeView = ConfigManager::FindActiveViewOrFirst(viewsCopy.Views, viewsCopy.ActiveViewId);
    if (activeView != nullptr && activeView->HideParents) {
        return;
    }

    // Chase the ancestor chain hop by hop within this sync: referenced − present at each hop,
    // deduped in first-seen order so every keyed request is deterministic. Each hop's fetch
    // yields the next hop's candidate refs (the parents' OWN `parent` field), so an
    // epic -> story -> task -> subtask chain resolves in one sync instead of one level per sync.
    // Stops when a hop finds nothing new, a fetch fails, or `kMaxParentFetchHops` is hit.
    std::vector<std::string> currentRefs = parentRefs;
    for (int hop = 0; hop < kMaxParentFetchHops; ++hop) {
        std::vector<std::string> missing;
        std::unordered_set<std::string> seen;
        for (const std::string& key : currentRefs) {
            if (workerKeepIds.count(key) == 0 && seen.insert(key).second) {
                missing.push_back(key);
            }
        }
        if (missing.empty()) {
            return;
        }

        LOG_INFO("TicketSyncService: Fetching %zu missing parent issue(s) (hop %d) for request ID=%llu", missing.size(),
                 hop, static_cast<unsigned long long>(reqId));
        // One token for the whole hop: the keyed fetch polls it inside the HTTP retry/backoff window
        // too, so a superseded sync stops mid-request rather than at the request's timeout.
        const std::function<bool()> cancelled = [this, reqId]() {
            return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
        };
        Result<HopRows, TrackerError> fetched =
            FetchHierarchyHop(cfgCopy, viewsCopy, cancelled, missing, /*childrenOfKeys=*/false, delta);
        if (!fetched.has_value() && (fetched.error().Kind == TrackerErrorKind::Cancelled || cancelled())) {
            return; // the sync's result is discarded; nothing to warn about
        }
        if (!fetched.has_value()) {
            MarkHierarchyWalkIncomplete(summary, std::to_string(missing.size()) +
                                                     " parent issue(s) could not be loaded: " + fetched.error().Detail);
            return;
        }
        HopRows hopRows = std::move(fetched.value());
        NoteRefusedKeys(summary, hopRows.Refused, "parent issue(s)");
        if (hopRows.Incomplete) {
            MarkHierarchyWalkIncomplete(summary, "some parent issue(s) listed by the tracker were not returned");
        }

        std::vector<std::string> nextRefs;
        for (const std::vector<CachedTicket>* rows : {&hopRows.Unchanged, &hopRows.Changed}) {
            for (const CachedTicket& parent : *rows) {
                if (!parent.id.empty()) {
                    workerKeepIds.insert(parent.id);
                }
                std::string grandparentKey = ParentHierarchyPure::ParentKeyOf(parent);
                if (!grandparentKey.empty()) {
                    nextRefs.push_back(std::move(grandparentKey));
                }
            }
        }
        summary.FetchedCount += hopRows.Changed.size();
        if (!QueueBatch(reqId, std::move(hopRows.Unchanged), /*cachedUnchanged=*/true) ||
            !QueueBatch(reqId, std::move(hopRows.Changed), /*cachedUnchanged=*/false)) {
            return;
        }

        if (nextRefs.empty()) {
            return;
        }
        currentRefs = std::move(nextRefs);
    }
    if (AnyKeyNotLoaded(currentRefs, workerKeepIds)) {
        MarkHierarchyWalkIncomplete(summary, "parent chain deeper than " + std::to_string(kMaxParentFetchHops) +
                                                 " levels; the levels above are not loaded");
    }
}

void TicketSyncService::FetchChildrenIntoQueue(std::uint64_t reqId, const TrackerConfig& cfgCopy,
                                               const ViewsStore& viewsCopy, const std::vector<std::string>& streamedIds,
                                               std::unordered_set<std::string>& workerKeepIds,
                                               TrackerIssueFetchSummary& summary, const HierarchyDelta* delta) {
    if (streamedIds.empty()) {
        return;
    }
    // Global opt-out (Preferences -> Editing -> Grid behaviour -> Load parent issues) — reused
    // here for the downward direction too: one switch controls both extra-fetch shapes.
    if (!cfgCopy.LoadParentIssues) {
        return;
    }
    // Deliberately NOT gated on the active view's HideParents, unlike FetchMissingParentsIntoQueue.
    // That toggle drops rows that are themselves a parent of another visible row — exactly the
    // leaf descendants this fetch supplies are what survive the filter, so skipping the fetch
    // here would defeat the toggle (an epic-only view with hide-parents on would show nothing)
    // rather than save work the toggle made pointless.

    // Chase the descendant tree hop by hop within this sync: every ticket streamed by the
    // view's own JQL/filter is a candidate parent. Each hop's fetched children become the next
    // hop's candidate parents (deduped), so an epic -> story -> task -> subtask tree resolves
    // in one sync from a view that only matched the epic. Stops when a hop finds no children,
    // a fetch fails, or `kMaxChildFetchHops` is hit.
    std::vector<std::string> currentParentKeys = streamedIds;
    // Keys already asked about in this sync, across hops: a child already queried as a parent (every
    // streamed id is one) is not asked about again, so a view that already holds the whole tree stops
    // after one hop instead of re-downloading it level by level.
    std::unordered_set<std::string> queriedParents;
    for (int hop = 0; hop < kMaxChildFetchHops; ++hop) {
        {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            if (activeStreamingSync_.RequestId != reqId || activeStreamingSync_.Cancelled) {
                return;
            }
        }
        std::vector<std::string> keys;
        for (const std::string& key : currentParentKeys) {
            if (queriedParents.insert(key).second) {
                keys.push_back(key);
            }
        }
        if (keys.empty()) {
            return;
        }

        LOG_INFO("TicketSyncService: Fetching children of %zu ticket(s) (hop %d) for request ID=%llu", keys.size(), hop,
                 static_cast<unsigned long long>(reqId));
        const std::function<bool()> cancelled = [this, reqId]() {
            return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
        };
        Result<HopRows, TrackerError> fetched =
            FetchHierarchyHop(cfgCopy, viewsCopy, cancelled, keys, /*childrenOfKeys=*/true, delta);
        if (!fetched.has_value() && (fetched.error().Kind == TrackerErrorKind::Cancelled || cancelled())) {
            return; // the sync's result is discarded; nothing to warn about
        }
        if (!fetched.has_value()) {
            MarkHierarchyWalkIncomplete(summary, "children of " + std::to_string(keys.size()) +
                                                     " issue(s) could not be loaded: " + fetched.error().Detail);
            return;
        }
        HopRows hopRows = std::move(fetched.value());
        NoteRefusedKeys(summary, hopRows.Refused, "issue(s) in the children walk");
        if (hopRows.Incomplete) {
            MarkHierarchyWalkIncomplete(summary, "some child issue(s) listed by the tracker were not returned");
        }

        std::vector<CachedTicket> newUnchanged;
        std::vector<CachedTicket> newChanged;
        std::vector<std::string> nextParentKeys;
        nextParentKeys.reserve(hopRows.Unchanged.size() + hopRows.Changed.size());
        const auto sift = [&](std::vector<CachedTicket>& rows, std::vector<CachedTicket>& fresh) {
            for (CachedTicket& child : rows) {
                if (child.id.empty()) {
                    continue;
                }
                // Every fetched child is a candidate parent for the NEXT hop regardless of whether
                // it was already cached - a ticket cached by an earlier sync may still have children
                // this sync hasn't chased yet. Only genuinely new tickets get queued as a batch.
                nextParentKeys.push_back(child.id);
                if (workerKeepIds.insert(child.id).second) {
                    fresh.push_back(std::move(child));
                }
            }
        };
        sift(hopRows.Unchanged, newUnchanged);
        sift(hopRows.Changed, newChanged);
        summary.FetchedCount += newChanged.size();
        if (!QueueBatch(reqId, std::move(newUnchanged), /*cachedUnchanged=*/true) ||
            !QueueBatch(reqId, std::move(newChanged), /*cachedUnchanged=*/false)) {
            return;
        }

        if (nextParentKeys.empty()) {
            return;
        }
        currentParentKeys = std::move(nextParentKeys);
    }
    // Keys already asked about cannot hold unseen children; only an unasked one means the cap cut it.
    if (AnyKeyNotLoaded(currentParentKeys, queriedParents)) {
        MarkHierarchyWalkIncomplete(summary, "descendant tree deeper than " + std::to_string(kMaxChildFetchHops) +
                                                 " levels; the levels below are not loaded");
    }
}

// ---- Delta reload (docs/plans/active/delta-sync.md § Phase 3) ---------------------------------------

namespace {
/// Ticket downloads of one delta attempt, in refusal-isolating batches (a key the tracker refuses is
/// reported in `refused` instead of failing the rest).
Result<std::vector<CachedTicket>, TrackerError> DownloadChangedRows(ITrackerIssueReader& backend,
                                                                    const TrackerConfig& cfg, const ViewsStore& views,
                                                                    const std::function<bool()>& cancelled,
                                                                    const std::vector<std::string>& keys,
                                                                    std::vector<std::string>& refused) {
    if (keys.empty()) {
        return Result<std::vector<CachedTicket>, TrackerError>::Ok(std::vector<CachedTicket>());
    }
    int budget = RefusalSplitBudget(keys.size());
    return FetchKeysIsolatingRefusals(
        [&](const std::vector<std::string>& batch) {
            return backend.FetchIssuesForKeysCancellable(cfg, batch, views, cancelled);
        },
        cancelled, keys, budget, refused);
}
} // namespace

struct TicketSyncService::DeltaWork {
    TrackerRoster Roster;
    smatchet::sync::DeltaFetchSet Set;
    std::vector<CachedTicket> Unchanged;                  // served from the cache
    std::vector<CachedTicket> Changed;                    // downloaded this session
    std::unordered_map<std::string, TicketSyncMeta> Meta; // cached versions the attempt started from
    std::vector<std::string> RequiredFields;              // fields the active view needs a cached row to carry
    std::int64_t NowSec = 0;
};

namespace {
/// Splits the roster, reads the unchanged rows back from the cache, and moves any row the cache cannot
/// produce into the download list. The rows to use are `unchangedRows`, never set.Unchanged (which still lists
/// the keys the cache could not serve; they are also appended to ToFetch).
void ServeUnchangedRows(ISyncCache& cache, const std::string& backendKey,
                        const std::unordered_map<std::string, TicketSyncMeta>& meta,
                        const std::vector<std::string>& requiredFields, std::int64_t nowSec, std::int64_t maxRowAgeSec,
                        const TrackerRoster& roster, smatchet::sync::DeltaFetchSet& set,
                        std::vector<CachedTicket>& unchangedRows) {
    set = smatchet::sync::ComputeDeltaFetchSet(roster.Entries, meta, requiredFields, nowSec, maxRowAgeSec);
    unchangedRows = cache.GetTicketsByIds(backendKey, set.Unchanged);
    std::unordered_set<std::string> served;
    for (const CachedTicket& row : unchangedRows) {
        served.insert(row.id);
    }
    for (const std::string& key : set.Unchanged) {
        if (served.find(key) == served.end()) {
            set.ToFetch.push_back(key); // the cache lost the row: download it
        }
    }
}

/// ServeUnchangedRows plus the view-level threshold. Returns the reason to abandon the delta attempt, or empty.
std::string PlanDeltaRows(ISyncCache& cache, const std::string& backendKey,
                          const std::unordered_map<std::string, TicketSyncMeta>& meta,
                          const std::vector<std::string>& requiredFields, std::int64_t nowSec,
                          std::int64_t maxRowAgeSec, const TrackerRoster& roster, smatchet::sync::DeltaFetchSet& set,
                          std::vector<CachedTicket>& unchangedRows) {
    ServeUnchangedRows(cache, backendKey, meta, requiredFields, nowSec, maxRowAgeSec, roster, set, unchangedRows);
    if (smatchet::sync::ShouldPreferFullFetch(roster.Entries.size(), set.ToFetch.size())) {
        return "most of the view changed (" + std::to_string(set.ToFetch.size()) + " of " +
               std::to_string(roster.Entries.size()) + " issues)";
    }
    return std::string();
}

/// Keys the tracker refused on an earlier delta attempt leave the download list; their cached rows stand in (the
/// body is unreadable anyway). The key then counts as returned, so the session stays authoritative and raises no
/// caveat on every sync for a ticket that stays refused. A key whose roster version is newer than the one it was
/// refused for is asked for again: the ticket changed since, and may be servable now.
void ServeRememberedRefusals(ISyncCache& cache, const std::string& backendKey,
                             const smatchet::sync::RefusedKeySet& refusedKeys, const TrackerRoster& roster,
                             smatchet::sync::DeltaFetchSet& set, std::vector<CachedTicket>& unchangedRows) {
    if (refusedKeys.Empty()) {
        return;
    }
    std::unordered_map<std::string, const std::string*> listedVersion;
    for (const TrackerRosterEntry& entry : roster.Entries) {
        listedVersion.emplace(entry.Key, &entry.Updated);
    }
    const std::string noVersion;
    std::vector<std::string> kept;
    std::vector<std::string> skipped;
    for (std::string& key : set.ToFetch) {
        const auto listed = listedVersion.find(key);
        const bool skip = refusedKeys.ShouldSkip(key, listed != listedVersion.end() ? *listed->second : noVersion);
        (skip ? skipped : kept).push_back(std::move(key));
    }
    set.ToFetch.swap(kept);
    if (skipped.empty()) {
        return;
    }
    LOG_DEBUG("TicketSyncService: not asking again for %zu key(s) the tracker refused earlier", skipped.size());
    for (CachedTicket& row : ReadCachedCopies(&cache, backendKey, skipped)) {
        unchangedRows.push_back(std::move(row));
    }
}

// Remembers `refused` with the version the roster listed for each key, so a later fetch that serves the body at that
// version (or a newer one) can forget the refusal again.
void RememberRefusals(smatchet::sync::RefusedKeySet& set, const std::vector<std::string>& refused,
                      const TrackerRoster& roster, std::int64_t nowSec) {
    std::unordered_map<std::string, std::string> listedVersion;
    for (const TrackerRosterEntry& entry : roster.Entries) {
        listedVersion.emplace(entry.Key, entry.Updated);
    }
    for (const std::string& key : refused) {
        const auto listed = listedVersion.find(key);
        set.Insert(key, listed != listedVersion.end() ? listed->second : std::string(), nowSec);
    }
}

} // namespace

void TicketSyncService::FailDeltaSession(std::uint64_t reqId, const std::string& message) {
    LOG_ERROR("TicketSyncService: %s", message.c_str());
    std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
    if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
        activeStreamingSync_.FetchError = message;
        activeStreamingSync_.FetchErrorTransient = false; // a bug shape: the ask-the-user branch is the safe one
        activeStreamingSync_.FullSyncCompleted = false;
    }
}

TicketSyncService::DeltaOutcome TicketSyncService::RunDeltaFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy,
                                                                 const ViewsStore& viewsCopy) {
    DeltaOutcome out;
    const std::function<bool()> cancelled = [this, reqId]() {
        return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
    };
    ISyncCache* cache = deps_.Cache();
    ITrackerIssueReader* backend = deps_.Backend();
    if (cache == nullptr || backend == nullptr) {
        out.FallbackReason = "no cache or backend";
        return out;
    }
    const std::string backendKey = deps_.CacheBackendKey();
    // The cached versions this session's worker already loaded (RunStreamingWorkerBody, same thread): one read of
    // the table per session. A failed read never reaches here (the worker body reports it as its own fallback).
    const std::unordered_map<std::string, TicketSyncMeta>& meta = sessionMeta_;

    smatchet::sync::SyncStrategyInput input;
    input.Requested = smatchet::sync::SyncMode::Delta;
    input.DeltaEnabled = cfgCopy.DeltaSyncEnabled;
    input.BackendSupportsRoster = backend->SupportsVersionedRoster();
    input.HaveAnyCachedMeta = !meta.empty();
    const smatchet::sync::SyncStrategyChoice choice = smatchet::sync::ChooseSyncStrategy(input);
    if (choice.Mode == smatchet::sync::SyncMode::Full) {
        out.FallbackReason = choice.FallbackReason;
        out.ExpectedFallback = true;
        return out;
    }

    Result<TrackerRoster, TrackerError> roster = backend->FetchViewRoster(cfgCopy, viewsCopy, cancelled);
    if (!roster.has_value()) {
        if (roster.error().Kind == TrackerErrorKind::Cancelled || cancelled()) {
            out.Finished = true; // cancelled: the session's result is discarded, no fallback
        } else {
            out.FallbackReason = "roster request failed: " + roster.error().Detail;
        }
        return out;
    }

    if (!roster.value().Entries.empty() && !smatchet::sync::AnyRosterKeyHasCachedMeta(roster.value().Entries, meta)) {
        // The cache knows versions, but none for this view (another pane's rows): every row would download as new.
        out.FallbackReason = "no cached ticket versions for this view yet";
        out.ExpectedFallback = true;
        return out;
    }

    DeltaWork work;
    work.Roster = std::move(roster.value());
    work.Meta = meta;
    work.NowSec = deps_.NowUnixSeconds();
    const ViewDefinition* activeView = ConfigManager::FindActiveViewOrFirst(viewsCopy.Views, viewsCopy.ActiveViewId);
    work.RequiredFields =
        smatchet::sync::RequiredFieldsOfView(activeView != nullptr ? activeView->Fields : std::vector<std::string>());
    out.FallbackReason = PlanDeltaRows(*cache, backendKey, meta, work.RequiredFields, work.NowSec,
                                       cfgCopy.DeltaSyncMaxRowAgeSec, work.Roster, work.Set, work.Unchanged);
    if (!out.FallbackReason.empty()) {
        return out;
    }
    if (cancelled()) {
        out.Finished = true;
        return out;
    }

    // Keys refused on an earlier attempt are not asked for again until the refusal is old enough to retry.
    refusedKeys_.ExpireOlderThan(work.NowSec, cfgCopy.DeltaSyncMaxRowAgeSec);
    ServeRememberedRefusals(*cache, backendKey, refusedKeys_, work.Roster, work.Set, work.Unchanged);
    std::vector<std::string> refused;
    Result<std::vector<CachedTicket>, TrackerError> downloaded =
        DownloadChangedRows(*backend, cfgCopy, viewsCopy, cancelled, work.Set.ToFetch, refused);
    if (!downloaded.has_value()) {
        if (downloaded.error().Kind == TrackerErrorKind::Cancelled || cancelled()) {
            out.Finished = true;
        } else {
            out.FallbackReason = "ticket download failed: " + downloaded.error().Detail;
        }
        return out;
    }
    if (!refused.empty()) {
        // The roster just listed these keys, so a 400/404/410-style refusal is not proof the ticket is gone (a proxy
        // limit, a permission blip, an oversized request look the same): nothing is purged on its strength, the full
        // fetch decides. The keys are remembered so one that stays refused does not repeat this on every attempt.
        RememberRefusals(refusedKeys_, refused, work.Roster, work.NowSec);
        out.FallbackReason = std::to_string(refused.size()) + " roster-listed issue(s) refused on download (" +
                             refused.front() + (refused.size() > 1 ? ", ..." : "") + ")";
        return out;
    }
    work.Changed = std::move(downloaded.value());
    // From the first queued row on, a second full queue would double the session's rows: whatever goes wrong
    // inside FinishDeltaFetch ends the session instead of falling back.
    try {
        FinishDeltaFetch(reqId, cfgCopy, viewsCopy, work);
    } catch (const std::exception& ex) {
        FailDeltaSession(reqId, std::string("delta reload failed while applying: ") + ex.what());
    } catch (...) {
        FailDeltaSession(reqId, "delta reload failed while applying (unknown exception)");
    }
    out.Finished = true;
    return out;
}

void TicketSyncService::FinishDeltaFetch(std::uint64_t reqId, const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                         DeltaWork& work) {
    // Keep set: every row actually obtained (served from the cache or downloaded), plus roster keys that came
    // back with neither a row nor a refusal. Those are not proven gone, so they are kept - but the walk is then
    // not authoritative (FullSyncCompleted off), so nothing is purged on the strength of an incomplete answer.
    std::unordered_set<std::string> obtained;
    for (const std::vector<CachedTicket>* rows : {&work.Unchanged, &work.Changed}) {
        for (const CachedTicket& row : *rows) {
            obtained.insert(row.id);
        }
    }
    std::unordered_set<std::string> keepIds;
    std::vector<std::string> streamedIds;
    std::size_t listedButMissing = 0;
    for (const TrackerRosterEntry& entry : work.Roster.Entries) {
        if (entry.Key.empty() || !keepIds.insert(entry.Key).second) {
            continue;
        }
        streamedIds.push_back(entry.Key);
        if (obtained.find(entry.Key) == obtained.end()) {
            ++listedButMissing;
        }
    }
    std::vector<std::string> parentRefs;
    for (const std::vector<CachedTicket>* rows : {&work.Unchanged, &work.Changed}) {
        for (const CachedTicket& row : *rows) {
            std::string parentKey = ParentHierarchyPure::ParentKeyOf(row);
            if (!parentKey.empty()) {
                parentRefs.push_back(std::move(parentKey));
            }
        }
    }

    TrackerIssueFetchSummary summary;
    summary.FetchedCount = work.Changed.size();
    summary.FullSyncCompleted = work.Roster.Complete && listedButMissing == 0;
    summary.AppendWarning(work.Roster.Warning);
    if (listedButMissing > 0) {
        summary.AppendWarning(std::to_string(listedButMissing) + " issue(s) listed by the tracker were not returned");
    }

    smatchet::sync::SyncSessionStats partial;
    partial.Mode = "delta";
    partial.RosterSize = work.Roster.Entries.size();
    partial.Unchanged = work.Unchanged.size();

    const auto cancelled = [this, reqId]() {
        return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
    };
    try {
        if (!QueueBatch(reqId, std::move(work.Unchanged), /*cachedUnchanged=*/true) ||
            !QueueBatch(reqId, std::move(work.Changed), /*cachedUnchanged=*/false)) {
            return; // superseded while queueing: the result is discarded
        }
    } catch (const std::exception& ex) {
        // A failed save (SQLite) of the changed rows: some rows may be queued already, so no fallback and no
        // purge. The session publishes what it has and says so.
        summary.FullSyncCompleted = false;
        summary.AppendWarning(std::string("some changed issue(s) could not be saved: ") + ex.what());
        LOG_WARN("TicketSyncService: delta queue threw: %s", ex.what());
        PublishWorkerResult(reqId, summary, keepIds, partial, streamedIds);
        return;
    }
    // Rows are queued: from here a failure must NOT fall back to a second full queue. A throwing top-up just
    // makes the result non-authoritative (no purge) and the session still publishes.
    // The parent and child top-ups are delta too: they list versions first and download only changed rows.
    HierarchyDelta hierarchyDelta;
    hierarchyDelta.Meta = &work.Meta;
    hierarchyDelta.RequiredFields = work.RequiredFields;
    hierarchyDelta.NowSec = work.NowSec;
    hierarchyDelta.MaxAgeSec = cfgCopy.DeltaSyncMaxRowAgeSec;
    try {
        if (!cancelled()) {
            FetchMissingParentsIntoQueue(reqId, cfgCopy, viewsCopy, parentRefs, keepIds, summary, &hierarchyDelta);
        }
        if (!cancelled()) {
            FetchChildrenIntoQueue(reqId, cfgCopy, viewsCopy, streamedIds, keepIds, summary, &hierarchyDelta);
        }
    } catch (const std::exception& ex) {
        summary.FullSyncCompleted = false;
        summary.AppendWarning(std::string("hierarchy top-up failed: ") + ex.what());
        LOG_WARN("TicketSyncService: delta top-up threw: %s", ex.what());
    }
    PublishWorkerResult(reqId, summary, keepIds, partial, std::move(streamedIds));
}

Result<TicketSyncService::HopRows, TrackerError>
TicketSyncService::FetchHierarchyHop(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy,
                                     const std::function<bool()>& cancelled, const std::vector<std::string>& keys,
                                     bool childrenOfKeys, const HierarchyDelta* delta) {
    using HopResult = Result<HopRows, TrackerError>;
    ITrackerIssueReader* backend = deps_.Backend();
    ISyncCache* cache = deps_.Cache();
    HopRows rows;

    if (delta != nullptr && delta->Meta != nullptr && backend != nullptr && cache != nullptr) {
        Result<TrackerRoster, TrackerError> roster = childrenOfKeys
                                                         ? backend->FetchChildRosterOfKeys(cfgCopy, keys, cancelled)
                                                         : backend->FetchRosterForKeys(cfgCopy, keys, cancelled);
        if (!roster.has_value() && (roster.error().Kind == TrackerErrorKind::Cancelled || cancelled())) {
            return HopResult::Err(TrackerErrorCancelled());
        }
        if (roster.has_value() && roster.value().Complete) {
            smatchet::sync::DeltaFetchSet set;
            ServeUnchangedRows(*cache, deps_.CacheBackendKey(), *delta->Meta, delta->RequiredFields, delta->NowSec,
                               delta->MaxAgeSec, roster.value(), set, rows.Unchanged);
            std::vector<std::string> refused;
            Result<std::vector<CachedTicket>, TrackerError> downloaded =
                DownloadChangedRows(*backend, cfgCopy, viewsCopy, cancelled, set.ToFetch, refused);
            if (downloaded.has_value()) {
                rows.Changed = std::move(downloaded.value());
                rows.Refused = std::move(refused);
                // A key the roster listed that came back with no row, or that the tracker refused, is not proven
                // gone (a refusal of a roster-listed key is never authoritative): the walk must not purge on its
                // strength. The refused keys are still named in the warning.
                rows.Incomplete = !rows.Refused.empty() || set.ToFetch.size() > rows.Changed.size();
                return HopResult::Ok(std::move(rows));
            }
            if (downloaded.error().Kind == TrackerErrorKind::Cancelled || cancelled()) {
                return HopResult::Err(TrackerErrorCancelled());
            }
            rows = HopRows(); // fall through to the full-body hop
        }
        // A roster error or an incomplete roster: today's full-body hop below (it isolates refused keys).
        LOG_INFO("TicketSyncService: %s roster for %zu key(s) unusable (%s); downloading full bodies for this hop.",
                 childrenOfKeys ? "child" : "parent", keys.size(),
                 roster.has_value() ? "incomplete" : roster.error().Detail.c_str());
    }

    std::vector<std::string> refused;
    int budget = RefusalSplitBudget(keys.size());
    Result<std::vector<CachedTicket>, TrackerError> fetched = FetchKeysIsolatingRefusals(
        [&](const std::vector<std::string>& batch) {
            return childrenOfKeys ? backend->FetchChildrenOfKeysCancellable(cfgCopy, batch, viewsCopy, cancelled)
                                  : backend->FetchIssuesForKeysCancellable(cfgCopy, batch, viewsCopy, cancelled);
        },
        cancelled, keys, budget, refused);
    if (!fetched.has_value()) {
        return HopResult::Err(fetched.error());
    }
    rows.Changed = std::move(fetched.value());
    rows.Refused = std::move(refused);
    return HopResult::Ok(std::move(rows));
}
