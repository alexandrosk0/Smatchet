#pragma once

// AppControllerDepsAdapter — concrete implementation of `IOfflineQueueDeps` and
// `ITicketSyncDeps` that forwards every method to a live `AppController&`. Owned by
// AppController via `std::unique_ptr`; constructed in `AppController::Initialize` and
// destroyed before any AppController member it forwards to.
//
// Why two interfaces, one adapter: both interfaces surface overlapping AppController state
// (`Cache`, `Backend`, the connectivity-banner / deferred-notify pair). Implementing both on a
// single adapter keeps the wiring trivial — AppController owns one adapter, passes the same
// pointer to both services via a different interface upcast. Tests substitute two independent
// fake adapters (`FakeOfflineQueueDeps`, `FakeTicketSyncDeps`) so a unit test of one service
// is never forced to stub methods the other service touches.

#include "IOfflineQueueDeps.h"
#include "ITicketSyncDeps.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ITrackerClient.h"

class AppController;
class LocalCacheManager;
class ITrackerConnectivity;
class ITrackerBackendFactory;
struct TrackerConfig;

class AppControllerDepsAdapter : public IOfflineQueueDeps, public ITicketSyncDeps {
  public:
    explicit AppControllerDepsAdapter(AppController& app);

    // ---- IOfflineQueueDeps ------------------------------------------------------------
    LocalCacheManager* Cache() override;
    ITrackerClient* Backend() override;
    const std::vector<TrackerField>& AvailableFields() const override;
    RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                         const std::string& issueTypeName) const override;
    void LaunchBackgroundTask(std::function<void()> task) override;
    void RefreshLocalData() override;
    void RequestDeferredLiveTrackerBackendSuccessNotify() override;

    // ---- ITicketSyncDeps --------------------------------------------------------------
    // `Cache()` and `Backend()` are already declared above; they're shared between the two
    // interfaces. The compiler resolves the diamond via the single overriding method.
    ITrackerConnectivity* BackendConnectivity() override;
    void SetBackend(std::unique_ptr<ITrackerClient> backend) override;
    ITrackerBackendFactory* BackendFactory() override;
    void SetLastTrackerTicketSyncWarning(const std::string& message) override;
    void SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState state) override;
    void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) override;
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) override;
    // RequestDeferredLiveTrackerBackendSuccessNotify shared with IOfflineQueueDeps.
    std::mutex& ActiveTicketsMutex() override;
    std::vector<CachedTicket>& ActiveTickets() override;
    void SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) override;
    void BumpActiveTicketsRevision() override;
    void PruneEditMetaCacheToActiveTickets() override;
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig cfg) override;
    void NotifyLuaTicketDataChanged() override;
    bool GetPendingLuaWindowBump() const override;
    void SetPendingLuaWindowBump(bool value) override;

  private:
    AppController& app_;
};
