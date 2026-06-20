#include "AppController.h"

#include "ConnectivityMonitorService.h"

#include <chrono>
#include <string>

// Tracker connectivity surface (god-object decomposition Phase 3): the probe FSM, recovery latch,
// live ticket-sync warning, and the per-frame offline banner were lifted into
// ConnectivityMonitorService. This TU keeps the public AppController methods as thin delegators
// forwarding into `connectivity_`, preserving every signature the UI consumers + the adapter's
// re-pointed ITicketSyncDeps setters depend on. `connectivity_` is constructed eagerly in
// Initialize before the first frame tick, so every delegator below has a live target.

void AppController::TickTrackerConnectivityMonitor(const TrackerConfig& cfg) {
    if (connectivity_) {
        connectivity_->TickTrackerConnectivityMonitor(cfg);
    }
}

TrackerConnectivityBannerForUi
AppController::GetTrackerConnectivityBannerForUi(const std::string* sessionCatalogNote) const {
    if (connectivity_) {
        return connectivity_->GetBannerForUi(sessionCatalogNote);
    }
    return TrackerConnectivityBannerForUi{};
}

bool AppController::ConsumeTrackerConnectivityRecovery() {
    return connectivity_ ? connectivity_->ConsumeTrackerConnectivityRecovery() : false;
}

bool AppController::ConsumeFieldCatalogRefetchAfterLiveTicketSync() {
    return connectivity_ ? connectivity_->ConsumeFieldCatalogRefetchAfterLiveTicketSync() : false;
}

bool AppController::ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny() {
    return connectivity_ ? connectivity_->ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny() : false;
}

void AppController::requestDeferredLiveTrackerBackendSuccessNotify_() const {
    if (connectivity_) {
        connectivity_->RequestDeferredLiveTrackerBackendSuccessNotify();
    }
}

void AppController::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    if (connectivity_) {
        connectivity_->PushOfflineReplayTimersDuringTransportOutage(now);
    }
}

TrackerConnectivityState AppController::GetLastTrackerConnectivityState() const {
    return connectivity_ ? connectivity_->GetLastState() : TrackerConnectivityState::Unknown;
}

const std::string& AppController::GetLastTicketSyncWarning() const {
    static const std::string kEmpty;
    return connectivity_ ? connectivity_->LastTicketSyncWarning() : kEmpty;
}
