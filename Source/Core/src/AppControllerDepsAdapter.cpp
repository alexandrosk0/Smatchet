#include "AppControllerDepsAdapter.h"

#include "AppController.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
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

// Backend reads go through std::atomic_load so the shared_ptr-instance read can't race the
// live swap — atomic_store/atomic_exchange in SetBackend — and the returned raw subobject
// pointer stays valid because the swapped-out backend is retired via defer-free, not freed (ADR 0012).
ITrackerIssueReader* AppControllerDepsAdapter::Reader() {
    auto b = std::atomic_load(&app_.Backend);
    return b ? &b->Reader() : nullptr;
}

ITrackerIssueMutations* AppControllerDepsAdapter::Mutations() {
    auto b = std::atomic_load(&app_.Backend);
    return b ? b->Mutations() : nullptr;
}

ITrackerConnectivity* AppControllerDepsAdapter::BackendConnectivity() {
    auto b = std::atomic_load(&app_.Backend);
    return b ? &b->Connectivity() : nullptr;
}

const std::vector<TrackerField>& AppControllerDepsAdapter::AvailableFields() const { return app_.AvailableFields; }

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

ITrackerIssueReader* AppControllerDepsAdapter::Backend() {
    auto b = std::atomic_load(&app_.Backend);
    return b ? &b->Reader() : nullptr;
}

void AppControllerDepsAdapter::SetBackend(std::unique_ptr<ITrackerBackend> backend) {
    // Live tracker swap. atomic_exchange swaps the slot AND returns the old backend in one
    // synchronised step — off-thread workers read app_.Backend via std::atomic_load, and a plain
    // assignment would data-race the shared_ptr instance in C++14). The old backend is then
    // RETIRED (defer-free), not freed, so any raw Reader/Mutations/Connectivity pointer a worker
    // captured before the swap stays valid until shutdown. See ADR 0012.
    std::shared_ptr<ITrackerBackend> incoming(std::move(backend));
    std::shared_ptr<ITrackerBackend> old = std::atomic_exchange(&app_.Backend, incoming);
    app_.RetireBackend(std::move(old));
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

void AppControllerDepsAdapter::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    app_.PushOfflineReplayTimersDuringTransportOutage(now);
}

std::mutex& AppControllerDepsAdapter::ActiveTicketsMutex() { return app_.activeTicketsMutex_; }

std::vector<CachedTicket>& AppControllerDepsAdapter::ActiveTickets() { return app_.ActiveTickets; }

void AppControllerDepsAdapter::SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) {
    app_.activeTicketsPublished_ = std::move(snap);
}

void AppControllerDepsAdapter::BumpActiveTicketsRevision() { app_.ActiveTicketsRevision.fetch_add(1); }

void AppControllerDepsAdapter::PruneEditMetaCacheToActiveTickets() { app_.PruneEditMetaCacheToActiveTickets(); }

void AppControllerDepsAdapter::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig cfg) {
    app_.WarmIssueTypeEditMetaAtStartAsync(std::move(cfg));
}

void AppControllerDepsAdapter::NotifyLuaTicketDataChanged() { app_.NotifyLuaTicketDataChanged(); }

bool AppControllerDepsAdapter::GetPendingLuaWindowBump() const { return app_.pendingLuaWindowBump_; }

void AppControllerDepsAdapter::SetPendingLuaWindowBump(bool value) { app_.pendingLuaWindowBump_ = value; }
