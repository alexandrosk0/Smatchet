#include "TicketSyncService.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "ITicketSyncDeps.h"
#include "ITrackerBackendFactory.h"
#include "ISyncCache.h"
#include "Logger.h"
#include "StringUtil.h"
#include "Sync/TicketRosterFilterPure.h" // shared keep-set roster filter (also used by the local-data refresh)
#include "Views.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

TicketSyncService::TicketSyncService(ITicketSyncDeps& deps) : deps_(deps) {}

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

void TicketSyncService::CancelAndJoinActiveStreamingSync() {
    activeStreamingSync_.Cancelled = true;
    activeStreamingSync_.Superseded = true;

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
    size_t saved = 0;
    for (const auto& t : freshTickets) {
        deps_.Cache()->SaveTicket(backendKey, t);
        ++saved;
    }

    size_t deleted = 0;
    // Empty-fetch guard: a full-sync that returns zero tickets cannot prove the cache is
    // stale — treating it as authoritative would wipe every row silently (e.g. a 200-with-
    // empty-body network glitch). Once the empty result has repeated kEmptyFullSyncWipeThreshold
    // times AND persisted kEmptyFullSyncMinStreakElapsed we trust it, so genuinely-empty projects
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
                                                   kEmptyFullSyncMinStreakElapsed)) {
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
    // 1. Drain a superseded session, else start a pending request once the previous session
    //    (worker + apply queue + stale cleanup) is fully drained.
    const bool isWorkerActive = activeStreamingSync_.Active.load();
    const bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load();

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

    if (hasPendingSyncRequest_) {
        hasPendingSyncRequest_ = false;
        StartStreamingSync(pendingConfig_, pendingViews_);
    }
    return true;
}

bool TicketSyncService::StartPendingSyncIfIdle(bool /*isSessionBusy*/) {
    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }
    if (hasPendingSyncRequest_) {
        hasPendingSyncRequest_ = false;
        StartStreamingSync(pendingConfig_, pendingViews_);
        return true;
    }
    return false;
}

bool TicketSyncService::DrainStaleDeletionBudget() {
    auto start = std::chrono::high_resolution_clock::now();
    size_t deletedThisFrame = 0;
    bool inMemoryChanged = false;
    const std::string backendKey = deps_.CacheBackendKey();

    while (!staleIdsToDelete_.empty()) {
        std::string id = std::move(staleIdsToDelete_.back());
        staleIdsToDelete_.pop_back();

        if (deps_.Cache()) {
            deps_.Cache()->DeleteTicket(backendKey, id);
        }
        {
            std::lock_guard<std::mutex> lock(deps_.ActiveTicketsMutex());
            deps_.ActiveTickets().erase(std::remove_if(deps_.ActiveTickets().begin(), deps_.ActiveTickets().end(),
                                                       [&](const CachedTicket& t) { return t.id == id; }),
                                        deps_.ActiveTickets().end());
        }
        inMemoryChanged = true;
        deletedThisFrame++;
        staleDeletedSoFar_++;

        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        if (elapsed >= 3 || deletedThisFrame >= 10) {
            break;
        }
    }

    if (inMemoryChanged) {
        std::lock_guard<std::mutex> lock(deps_.ActiveTicketsMutex());
        deps_.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>(deps_.ActiveTickets()));
        deps_.BumpActiveTicketsRevision();
        // Stale-deletion shrinks visible state — windows that show ticket lists / counts
        // must re-record. Coalesced via pendingLuaWindowBump_ so a 200-id prune fires one
        // bump, not 200.
        deps_.SetPendingLuaWindowBump(true);
    }

    if (staleIdsToDelete_.empty()) {
        isDeletingStale_.store(false);
        LOG_INFO("TicketSyncService::TickStreamingApply finished stale deletion. total_deleted=%zu",
                 totalStaleToDelete_);
        // Trigger editmeta warmup after cleanup completes
        deps_.WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
        // Flush coalesced Lua window bump now that stale deletion finished. The session-
        // end flush above can't see this because stale-deletion runs across many frames
        // after the session-end block has already fired.
        if (deps_.GetPendingLuaWindowBump()) {
            deps_.NotifyLuaTicketDataChanged();
            deps_.SetPendingLuaWindowBump(false);
        }
    }
    return true;
}

void TicketSyncService::DrainPendingStreamingBatches() {
    auto start = std::chrono::high_resolution_clock::now();
    size_t ticketsProcessedInFrame = 0;
    bool stateChanged = false;
    std::vector<CachedTicket> batchToProcess;
    std::vector<CachedTicket> processedThisFrame;

    while (true) {
        batchToProcess.clear();
        {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            if (activeStreamingSync_.PendingBatches.empty()) {
                break;
            }
            auto& frontBatch = activeStreamingSync_.PendingBatches.front();
            if (frontBatch.empty()) {
                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());
                continue;
            }

            size_t sliceSize = (std::min)(frontBatch.size(), size_t(20 - ticketsProcessedInFrame));
            if (sliceSize == 0) {
                break; // Frame limit
            }

            const std::ptrdiff_t sliceOffset = static_cast<std::ptrdiff_t>(sliceSize);
            batchToProcess.insert(batchToProcess.end(), std::make_move_iterator(frontBatch.begin()),
                                  std::make_move_iterator(frontBatch.begin() + sliceOffset));
            frontBatch.erase(frontBatch.begin(), frontBatch.begin() + sliceOffset);

            if (frontBatch.empty()) {
                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());
            }
        }

        // Phase 3(a): persist the whole slice in ONE transaction instead of one-per-ticket
        // (docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 3a). KeepIds + the
        // per-frame counter stay a separate cheap loop.
        if (deps_.Cache()) {
            deps_.Cache()->SaveTickets(deps_.CacheBackendKey(), batchToProcess);
        }
        for (const auto& t : batchToProcess) {
            if (!t.id.empty()) {
                activeStreamingSync_.KeepIds.insert(t.id);
            }
            ticketsProcessedInFrame++;
        }
        processedThisFrame.insert(processedThisFrame.end(), std::make_move_iterator(batchToProcess.begin()),
                                  std::make_move_iterator(batchToProcess.end()));
        stateChanged = true;

        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start)
                .count();
        if (elapsed >= 3 || ticketsProcessedInFrame >= 20) {
            break;
        }
    }

    if (stateChanged) {
        {
            std::lock_guard<std::mutex> lock(deps_.ActiveTicketsMutex());
            for (const auto& t : processedThisFrame) {
                auto it = std::find_if(deps_.ActiveTickets().begin(), deps_.ActiveTickets().end(),
                                       [&](const CachedTicket& existing) { return existing.id == t.id; });
                if (it != deps_.ActiveTickets().end()) {
                    *it = t;
                } else {
                    deps_.ActiveTickets().push_back(t);
                }
            }
            deps_.SetActiveTicketsPublished(std::make_shared<const std::vector<CachedTicket>>(deps_.ActiveTickets()));
        }
        deps_.PruneEditMetaCacheToActiveTickets();
        deps_.BumpActiveTicketsRevision();
        // Streaming-batch end-of-batch — flip the coalesced bump; session-end emits once.
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

    SeedStaleDeletionForSession(fullSyncCompleted, keptThisSession);
    ConvergeActiveTicketsToSessionKeepSet(fullSyncCompleted, keptThisSession);

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
    // repeated kEmptyFullSyncWipeThreshold times AND held for kEmptyFullSyncMinStreakElapsed —
    // the count alone was satisfiable by two flaps seconds apart (#2143).
    const std::chrono::milliseconds streakElapsed = EmptyStreakElapsed();
    const bool skipWipe = ShouldSkipMassDeletionOnEmptyFullSync(
        keptThisSession, activeStreamingSync_.BackgroundStaleIds.size(), consecutiveEmptyFullSyncs_,
        kEmptyFullSyncWipeThreshold, streakElapsed, kEmptyFullSyncMinStreakElapsed);
    if (skipWipe) {
        LOG_WARN("TicketSyncService::TickStreamingApply skipped mass stale deletion on suspect empty full sync "
                 "(cached=%zu, consecutive_empty=%d, streak_held=%llds, required=%llds).",
                 activeStreamingSync_.BackgroundStaleIds.size(), consecutiveEmptyFullSyncs_,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(streakElapsed).count()),
                 static_cast<long long>(kEmptyFullSyncMinStreakElapsed.count()));
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

bool TicketSyncService::IsActive() const {
    return activeStreamingSync_.Active.load() || activeStreamingSync_.ActiveSessionRunning.load() ||
           isDeletingStale_.load();
}

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

void TicketSyncService::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {
    LOG_INFO("TicketSyncService::SyncWithBackend started (asynchronous streaming refresh).");

    TrackerConfig cfgCopy = configOverride ? *configOverride : ConfigManager::Load();
    ViewsStore viewsCopy = viewsOverride ? *viewsOverride : ConfigManager::LoadViewsOrBootstrap(cfgCopy);

    const bool isWorkerActive = activeStreamingSync_.Active.load();
    const bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load();

    if (isSessionBusy) {
        activeStreamingSync_.Cancelled = true;
        activeStreamingSync_.Superseded = true;
        hasPendingSyncRequest_ = true;
        pendingConfig_ = cfgCopy;
        pendingViews_ = viewsCopy;
        LOG_INFO("TicketSyncService: Active sync/apply/cleanup session busy. New sync request "
                 "deferred to avoid UI thread block.");
        return;
    }

    StartStreamingSync(cfgCopy, viewsCopy);
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
    }

    // Re-stamp the cache namespace to match the requested tracker (multi-grid Slice 1b).
    // Unconditional + idempotent: also covers a context whose key was never wired (e.g. a
    // test fixture) so the first sync writes under the right namespace, not under "".
    deps_.SetCacheBackendKey(ConfigManager::NormalizeViewsBackendKey(newTracker));

    // Backend-kind switch: clear in-memory tickets so the old backend's items don't
    // linger in the grid while the new backend's first fetch is in flight. Without this,
    // switching Jira → GitHub (or any cross-kind swap) leaves stale tickets visible and
    // the per-row update path tries to mutate them against the new backend, which fails
    // or silently writes to the wrong tracker. SQLite cache rows survive — switching
    // back later re-populates ActiveTickets via the cache hydrate path.
    if (backendSwapped) {
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
        LOG_INFO("TicketSyncService: Cleared in-memory ActiveTickets on backend-kind change.");
    }
}

void TicketSyncService::StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy) {
    if (activeStreamingSync_.WorkerThread.joinable()) {
        activeStreamingSync_.WorkerThread.join();
    }

    deps_.NotifySyncStatus("Syncing", "Refreshing issues from Tracker...", ITicketSyncDeps::SyncNotifyLevel::Info,
                           2500);

    SwapBackendIfTrackerChanged(cfgCopy);

    const std::uint64_t reqId = ++currentFetchRequestId_;
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
    }

    LOG_INFO("TicketSyncService: Spawning background worker for async streaming fetch request ID=%llu",
             static_cast<unsigned long long>(reqId));

    activeStreamingSync_.WorkerThread =
        std::thread([this, reqId, cfgCopy, viewsCopy]() { RunStreamingWorkerBody(reqId, cfgCopy, viewsCopy); });
}

void TicketSyncService::RunStreamingWorkerBody(std::uint64_t reqId, const TrackerConfig& cfgCopy,
                                               const ViewsStore& viewsCopy) {
    try {
        std::unordered_set<std::string> workerKeepIds;
        auto onBatch = [this, reqId, &workerKeepIds](std::vector<CachedTicket>&& batch) {
            for (const auto& ticket : batch) {
                if (!ticket.id.empty()) {
                    workerKeepIds.insert(ticket.id);
                }
            }
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
                activeStreamingSync_.PendingBatches.push_back(std::move(batch));
            }
        };
        auto shouldCancel = [this, reqId]() -> bool {
            return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;
        };

        TrackerIssueFetchSummary summary =
            deps_.Backend()->FetchIssuesStreamed(onBatch, shouldCancel, &cfgCopy, &viewsCopy);

        if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
            std::vector<std::string> localStaleIds;
            if (summary.FullSyncCompleted && deps_.Cache()) {
                // The deps getter hands back a mutex-guarded key copy — safe from this worker
                // thread; the key cannot change mid-session (swaps happen in StartStreamingSync,
                // which joins the previous worker first).
                std::vector<std::string> existingIds = deps_.Cache()->GetAllTicketIds(deps_.CacheBackendKey());
                std::copy_if(
                    existingIds.begin(), existingIds.end(), std::back_inserter(localStaleIds),
                    [&workerKeepIds](const std::string& id) { return workerKeepIds.find(id) == workerKeepIds.end(); });
            }

            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FullSyncCompleted = summary.FullSyncCompleted;
            activeStreamingSync_.FetchError = summary.FetchError;
            // Worker-side classification seam (N12): the kind the backend classified at its own
            // error site (summary.Error, item 12). Every backend fills it whenever FetchError is
            // set (Plane's resolve/exception paths closed in slice 3); an unclassified error is a
            // bug shape and lands on the safe non-transient branch.
            activeStreamingSync_.FetchErrorTransient = !summary.FetchError.empty() && summary.Error.IsRetryable();
            activeStreamingSync_.Warning = summary.Warning;
            activeStreamingSync_.TotalFetchedCount = summary.FetchedCount;
            if (summary.FullSyncCompleted && deps_.Cache()) {
                activeStreamingSync_.BackgroundStaleIds = std::move(localStaleIds);
            }
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("TicketSyncService: Worker thread caught exception: %s", ex.what());
        if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FetchError = std::string("Sync failed with exception: ") + ex.what();
            // Deliberately non-transient (13a precedent): transport failures surface as classified
            // statuses from the backends, never as throws — a thrown worker is a bug shape, and
            // the non-transient branch is the safe ask-the-user path.
            activeStreamingSync_.FetchErrorTransient = false;
            activeStreamingSync_.FullSyncCompleted = false;
        }
    } catch (...) {
        LOG_ERROR("TicketSyncService: Worker thread caught unknown exception.");
        if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FetchError = "Sync failed with unknown exception.";
            activeStreamingSync_.FetchErrorTransient = false; // fixed text — never transport-shaped
            activeStreamingSync_.FullSyncCompleted = false;
        }
    }

    activeStreamingSync_.Active = false;
}
