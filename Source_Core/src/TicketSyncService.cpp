#include "TicketSyncService.h"

#include "AppController.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "TrackerHttpUtils.h"

#include <chrono>
#include <mutex>
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
