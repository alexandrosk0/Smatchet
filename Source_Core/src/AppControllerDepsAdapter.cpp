#include "AppControllerDepsAdapter.h"

#include "AppController.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerClient.h"
#include "LocalCacheManager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

AppControllerDepsAdapter::AppControllerDepsAdapter(AppController& app) : app_(app) {}

// ---- IOfflineQueueDeps (and shared with ITicketSyncDeps) ------------------------------

LocalCacheManager* AppControllerDepsAdapter::Cache() { return app_.Cache.get(); }

ITrackerClient* AppControllerDepsAdapter::Backend() { return app_.Backend.get(); }

const std::vector<TrackerField>& AppControllerDepsAdapter::AvailableFields() const {
    return app_.AvailableFields;
}

RequiredFieldSet AppControllerDepsAdapter::GetRequiredFieldSet(const std::string& projectKey,
                                                                 const std::string& issueTypeId,
                                                                 const std::string& issueTypeName) const {
    return app_.GetRequiredFieldSet(projectKey, issueTypeId, issueTypeName);
}

void AppControllerDepsAdapter::LaunchBackgroundTask(std::function<void()> task) {
    app_.LaunchBackgroundTask(std::move(task));
}

void AppControllerDepsAdapter::RefreshLocalData() { app_.RefreshLocalData(); }

void AppControllerDepsAdapter::RequestDeferredLiveTrackerBackendSuccessNotify() {
    app_.requestDeferredLiveTrackerBackendSuccessNotify_();
}

// ---- ITicketSyncDeps ------------------------------------------------------------------

void AppControllerDepsAdapter::SetBackend(std::unique_ptr<ITrackerClient> backend) {
    app_.Backend = std::move(backend);
}

ITrackerBackendFactory* AppControllerDepsAdapter::BackendFactory() { return app_.backendFactory_.get(); }

void AppControllerDepsAdapter::SetLastTrackerTicketSyncWarning(const std::string& message) {
    app_.LastTrackerTicketSyncWarning = message;
}

void AppControllerDepsAdapter::SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState state) {
    app_.lastTrackerConnectivityState_ = static_cast<AppController::TrackerConnectivityState>(state);
}

void AppControllerDepsAdapter::SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) {
    app_.nextTrackerConnectivityProbeAt_ = at;
}

void AppControllerDepsAdapter::PushOfflineReplayTimersDuringTransportOutage(
    std::chrono::steady_clock::time_point now) {
    app_.PushOfflineReplayTimersDuringTransportOutage(now);
}

std::mutex& AppControllerDepsAdapter::ActiveTicketsMutex() { return app_.activeTicketsMutex_; }

std::vector<CachedTicket>& AppControllerDepsAdapter::ActiveTickets() { return app_.ActiveTickets; }

void AppControllerDepsAdapter::SetActiveTicketsPublished(
    std::shared_ptr<const std::vector<CachedTicket>> snap) {
    app_.activeTicketsPublished_ = std::move(snap);
}

void AppControllerDepsAdapter::BumpActiveTicketsRevision() { app_.ActiveTicketsRevision.fetch_add(1); }

void AppControllerDepsAdapter::PruneEditMetaCacheToActiveTickets() {
    app_.PruneEditMetaCacheToActiveTickets();
}

void AppControllerDepsAdapter::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig cfg) {
    app_.WarmIssueTypeEditMetaAtStartAsync(std::move(cfg));
}

void AppControllerDepsAdapter::NotifyLuaTicketDataChanged() { app_.NotifyLuaTicketDataChanged(); }

bool AppControllerDepsAdapter::GetPendingLuaWindowBump() const { return app_.pendingLuaWindowBump_; }

void AppControllerDepsAdapter::SetPendingLuaWindowBump(bool value) {
    app_.pendingLuaWindowBump_ = value;
}
