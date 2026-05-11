#include "TicketSyncService.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "SmatchetToast.h"
#include "TrackerHttpUtils.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

TicketSyncService::TicketSyncService(AppController& app) : app_(app) {}

void TicketSyncService::CancelAndJoinActiveStreamingSync() {
    app_.activeStreamingSync_.Cancelled = true;
    app_.activeStreamingSync_.Superseded = true;

    if (app_.activeStreamingSync_.WorkerThread.joinable()) {
        app_.activeStreamingSync_.WorkerThread.join();
    }

    app_.activeStreamingSync_.Active = false;
    app_.activeStreamingSync_.ActiveSessionRunning = false;

    {
        std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
        app_.activeStreamingSync_.PendingBatches.clear();
        app_.activeStreamingSync_.BackgroundStaleIds.clear();
    }
}

void TicketSyncService::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (!app_.Backend || !app_.Cache) {
        LOG_WARN("TicketSyncService::ApplyIssueFetchPack skipped: backend=%d cache=%d", app_.Backend ? 1 : 0,
                 app_.Cache ? 1 : 0);
        return;
    }

    app_.LastTrackerTicketSyncWarning.clear();

    const std::vector<CachedTicket>& freshTickets = pack.Tickets;
    const std::string& fetchError = pack.FetchError;
    const std::string& fetchWarning = pack.Warning;
    const bool fullSyncCompleted = pack.FullSyncCompleted;

    if (!fetchError.empty() && IsTrackerTransportErrorText(fetchError)) {
        app_.LastTrackerTicketSyncWarning = "Showing cached issues — live refresh did not complete: " + fetchError;
        LOG_WARN("TicketSyncService::ApplyIssueFetchPack transport-style fetch issue: %s", fetchError.c_str());
        app_.lastTrackerConnectivityState_ = AppController::TrackerConnectivityState::TransportDown;
        const auto nowProbe = std::chrono::steady_clock::now();
        app_.nextTrackerConnectivityProbeAt_ = nowProbe;
        app_.PushOfflineReplayTimersDuringTransportOutage(nowProbe);
    } else if (fetchError.empty()) {
        // Soft warnings still count as success — the fetched data is valid, just partial.
        if (!fetchWarning.empty()) {
            app_.LastTrackerTicketSyncWarning = "Sync completed with a caveat: " + fetchWarning;
            LOG_WARN("TicketSyncService::ApplyIssueFetchPack soft warning: %s", fetchWarning.c_str());
        }
        app_.requestDeferredLiveTrackerBackendSuccessNotify_();
    }

    size_t saved = 0;
    for (const auto& t : freshTickets) {
        app_.Cache->SaveTicket(t);
        ++saved;
    }

    size_t deleted = 0;
    if (fullSyncCompleted) {
        std::unordered_set<std::string> keepIds;
        keepIds.reserve(freshTickets.size());
        for (const auto& t : freshTickets) {
            if (!t.id.empty()) {
                keepIds.insert(t.id);
            }
        }
        std::vector<CachedTicket> existing = app_.Cache->GetAllTickets();
        for (const auto& row : existing) {
            if (keepIds.find(row.id) == keepIds.end()) {
                app_.Cache->DeleteTicket(row.id);
                ++deleted;
            }
        }
    }

    LOG_INFO("TicketSyncService::ApplyIssueFetchPack finished fetched=%zu saved=%zu deleted=%zu fullSync=%d",
             freshTickets.size(), saved, deleted, fullSyncCompleted ? 1 : 0);
}

void TicketSyncService::TickStreamingApply() {
    // 1. If we have a pending sync request and the previous session (worker + apply queue +
    //    stale cleanup) is fully drained, start it safely now.
    bool isWorkerActive = app_.activeStreamingSync_.Active.load();
    bool isSessionBusy = isWorkerActive || app_.activeStreamingSync_.ActiveSessionRunning ||
                         app_.isDeletingStale_.load();

    if (app_.activeStreamingSync_.Superseded.load()) {
        {
            std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
            app_.activeStreamingSync_.PendingBatches.clear();
            app_.activeStreamingSync_.BackgroundStaleIds.clear();
        }
        app_.staleIdsToDelete_.clear();
        app_.isDeletingStale_.store(false);
        app_.totalStaleToDelete_ = 0;
        app_.staleDeletedSoFar_ = 0;

        if (isWorkerActive) {
            return;
        }
        if (app_.activeStreamingSync_.WorkerThread.joinable()) {
            app_.activeStreamingSync_.WorkerThread.join();
        }
        app_.activeStreamingSync_.Active = false;
        app_.activeStreamingSync_.ActiveSessionRunning = false;
        app_.activeStreamingSync_.FullSyncCompleted = false;
        app_.activeStreamingSync_.TotalFetchedCount = 0;
        {
            // FetchError contract: every read/write through QueueMutex (worker joined above).
            std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
            app_.activeStreamingSync_.FetchError.clear();
            app_.activeStreamingSync_.Warning.clear();
        }
        app_.activeStreamingSync_.KeepIds.clear();
        app_.activeStreamingSync_.Cancelled = false;
        app_.activeStreamingSync_.Superseded = false;

        LOG_INFO("TicketSyncService::TickStreamingApply discarded superseded streaming sync session.");

        if (app_.hasPendingSyncRequest_) {
            app_.hasPendingSyncRequest_ = false;
            app_.StartStreamingSync(app_.pendingConfig_, app_.pendingViews_);
        }
        return;
    }

    if (!isSessionBusy) {
        if (app_.activeStreamingSync_.WorkerThread.joinable()) {
            app_.activeStreamingSync_.WorkerThread.join();
        }
        if (app_.hasPendingSyncRequest_) {
            app_.hasPendingSyncRequest_ = false;
            app_.StartStreamingSync(app_.pendingConfig_, app_.pendingViews_);
            return;
        }
    }

    // 2. Progressive, budgeted stale ticket deletion over multiple frames to avoid UI hitches.
    if (app_.isDeletingStale_.load()) {
        auto start = std::chrono::high_resolution_clock::now();
        size_t deletedThisFrame = 0;
        bool inMemoryChanged = false;

        while (!app_.staleIdsToDelete_.empty()) {
            std::string id = std::move(app_.staleIdsToDelete_.back());
            app_.staleIdsToDelete_.pop_back();

            if (app_.Cache) {
                app_.Cache->DeleteTicket(id);
            }
            {
                std::lock_guard<std::mutex> lock(app_.activeTicketsMutex_);
                app_.ActiveTickets.erase(std::remove_if(app_.ActiveTickets.begin(),
                                                        app_.ActiveTickets.end(),
                                                        [&](const CachedTicket& t) { return t.id == id; }),
                                          app_.ActiveTickets.end());
            }
            inMemoryChanged = true;
            deletedThisFrame++;
            app_.staleDeletedSoFar_++;

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::high_resolution_clock::now() - start)
                               .count();
            if (elapsed >= 3 || deletedThisFrame >= 10) {
                break;
            }
        }

        if (inMemoryChanged) {
            std::lock_guard<std::mutex> lock(app_.activeTicketsMutex_);
            app_.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(app_.ActiveTickets);
            app_.ActiveTicketsRevision.fetch_add(1);
        }

        if (app_.staleIdsToDelete_.empty()) {
            app_.isDeletingStale_.store(false);
            LOG_INFO("TicketSyncService::TickStreamingApply finished stale deletion. total_deleted=%zu",
                     app_.totalStaleToDelete_);
            // Trigger editmeta warmup after cleanup completes
            app_.WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
        }
        return;
    }

    if (!app_.activeStreamingSync_.ActiveSessionRunning) {
        return;
    }

    // 3. Process incoming streaming ticket batches in-memory and write to SQLite with budget.
    auto start = std::chrono::high_resolution_clock::now();
    size_t ticketsProcessedInFrame = 0;
    bool stateChanged = false;
    std::vector<CachedTicket> batchToProcess;
    std::vector<CachedTicket> processedThisFrame;

    while (true) {
        batchToProcess.clear();
        {
            std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
            if (app_.activeStreamingSync_.PendingBatches.empty()) {
                break;
            }
            auto& frontBatch = app_.activeStreamingSync_.PendingBatches.front();
            if (frontBatch.empty()) {
                app_.activeStreamingSync_.PendingBatches.erase(app_.activeStreamingSync_.PendingBatches.begin());
                continue;
            }

            size_t sliceSize = std::min(frontBatch.size(), size_t(20 - ticketsProcessedInFrame));
            if (sliceSize == 0) {
                break; // Frame limit
            }

            batchToProcess.insert(batchToProcess.end(), std::make_move_iterator(frontBatch.begin()),
                                  std::make_move_iterator(frontBatch.begin() + sliceSize));
            frontBatch.erase(frontBatch.begin(), frontBatch.begin() + sliceSize);

            if (frontBatch.empty()) {
                app_.activeStreamingSync_.PendingBatches.erase(app_.activeStreamingSync_.PendingBatches.begin());
            }
        }

        for (const auto& t : batchToProcess) {
            if (app_.Cache) {
                app_.Cache->SaveTicket(t);
            }
            if (!t.id.empty()) {
                app_.activeStreamingSync_.KeepIds.insert(t.id);
            }
            ticketsProcessedInFrame++;
        }
        processedThisFrame.insert(processedThisFrame.end(), std::make_move_iterator(batchToProcess.begin()),
                                  std::make_move_iterator(batchToProcess.end()));
        stateChanged = true;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::high_resolution_clock::now() - start)
                           .count();
        if (elapsed >= 3 || ticketsProcessedInFrame >= 20) {
            break;
        }
    }

    if (stateChanged) {
        {
            std::lock_guard<std::mutex> lock(app_.activeTicketsMutex_);
            for (const auto& t : processedThisFrame) {
                auto it = std::find_if(app_.ActiveTickets.begin(), app_.ActiveTickets.end(),
                                       [&](const CachedTicket& existing) { return existing.id == t.id; });
                if (it != app_.ActiveTickets.end()) {
                    *it = t;
                } else {
                    app_.ActiveTickets.push_back(t);
                }
            }
            app_.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(app_.ActiveTickets);
        }
        app_.PruneEditMetaCacheToActiveTickets();
        app_.ActiveTicketsRevision.fetch_add(1);
    }

    bool isWorkerFinished = !app_.activeStreamingSync_.Active.load();
    bool hasPending = false;
    {
        std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
        hasPending = !app_.activeStreamingSync_.PendingBatches.empty();
    }

    if (isWorkerFinished && !hasPending && app_.activeStreamingSync_.ActiveSessionRunning) {
        app_.activeStreamingSync_.ActiveSessionRunning = false;

        // FetchError + Warning are written by the worker thread under QueueMutex; acquire it
        // for the read. FullSyncCompleted is atomic and can be read without the lock.
        std::string fetchError;
        std::string fetchWarning;
        {
            std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
            fetchError = app_.activeStreamingSync_.FetchError;
            fetchWarning = app_.activeStreamingSync_.Warning;
        }

        bool fullSyncCompleted = app_.activeStreamingSync_.FullSyncCompleted;

        if (!fetchError.empty() && IsTrackerTransportErrorText(fetchError)) {
            app_.LastTrackerTicketSyncWarning =
                "Showing cached issues — live refresh did not complete: " + fetchError;
            LOG_WARN("TicketSyncService::TickStreamingApply transport-style fetch issue: %s", fetchError.c_str());
            app_.lastTrackerConnectivityState_ = AppController::TrackerConnectivityState::TransportDown;
            const auto nowProbe = std::chrono::steady_clock::now();
            app_.nextTrackerConnectivityProbeAt_ = nowProbe;
            app_.PushOfflineReplayTimersDuringTransportOutage(nowProbe);
            SmatchetToastManager::Instance().Push("Sync Failed", fetchError, ToastType::Error, 5000);
        } else if (!fetchError.empty()) {
            SmatchetToastManager::Instance().Push("Sync Warning", fetchError, ToastType::Warning, 5000);
        } else {
            // Soft warnings: data is good, just partial — still notify success but surface
            // the caveat as a warning banner + toast.
            if (!fetchWarning.empty()) {
                app_.LastTrackerTicketSyncWarning = "Sync completed with a caveat: " + fetchWarning;
                LOG_WARN("TicketSyncService::TickStreamingApply soft warning: %s", fetchWarning.c_str());
                SmatchetToastManager::Instance().Push("Sync Warning", fetchWarning, ToastType::Warning, 5000);
            }
            app_.requestDeferredLiveTrackerBackendSuccessNotify_();

            std::string msg =
                "Synchronized " + std::to_string(app_.activeStreamingSync_.KeepIds.size()) + " issues successfully.";
            SmatchetToastManager::Instance().Push("Sync Complete", msg, ToastType::Success, 4000);
        }

        app_.totalStaleToDelete_ = 0;
        app_.staleDeletedSoFar_ = 0;
        app_.staleIdsToDelete_.clear();

        if (fullSyncCompleted) {
            std::lock_guard<std::mutex> qLock(app_.activeStreamingSync_.QueueMutex);
            app_.staleIdsToDelete_ = std::move(app_.activeStreamingSync_.BackgroundStaleIds);

            if (!app_.staleIdsToDelete_.empty()) {
                app_.isDeletingStale_.store(true);
                app_.totalStaleToDelete_ = app_.staleIdsToDelete_.size();
                app_.staleDeletedSoFar_ = 0;
            }
        }

        LOG_INFO("TicketSyncService::TickStreamingApply finished sync session. saved_or_kept=%zu total_stale=%zu "
                 "fullSync=%d err=%s",
                 app_.activeStreamingSync_.KeepIds.size(), app_.totalStaleToDelete_, fullSyncCompleted ? 1 : 0,
                 fetchError.c_str());

        if (!app_.isDeletingStale_.load()) {
            app_.WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
        }
    }
}
