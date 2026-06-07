#pragma once

// GridContextDepsAdapter — concrete implementation of `IOfflineQueueDeps` and
// `ITicketSyncDeps` that forwards per-context methods (`Backend`/`SetBackend`/`ActiveTickets*`)
// to a `GridLiveContext&` and global methods (`Cache`, connectivity banner, offline-replay
// timers, Lua notify) to a live `AppController&`. Split from the former
// AppControllerDepsAdapter in multi-grid Slice 1 (ADR-0018): the adapter is the chokepoint
// that lets `ITicketSyncDeps` — and therefore TicketSyncService, its tests, and every
// external call site — stay unchanged while the engine state de-singletons.
// Owned by AppController via `std::unique_ptr`; constructed in `AppController::Initialize`
// and destroyed before any AppController member it forwards to (the GridLiveContext it
// references is declared after it in AppController, so the context dies first — the adapter
// never dereferences `ctx_` after that because the context's TicketSyncService, the only
// caller, dies with it).
// Why two interfaces, one adapter: both interfaces surface overlapping state
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

#include "ITrackerBackend.h"

class AppController;
class LocalCacheManager;
class ITrackerConnectivity;
class ITrackerIssueMutations;
class ITrackerIssueReader;
class ITrackerBackendFactory;
struct GridLiveContext;
struct TrackerConfig;

class GridContextDepsAdapter : public IOfflineQueueDeps, public ITicketSyncDeps {
  public:
    GridContextDepsAdapter(AppController& app, GridLiveContext& ctx);

    // ---- IOfflineQueueDeps ------------------------------------------------------------
    LocalCacheManager* Cache() override;
    ITrackerIssueReader* Reader() override;
    ITrackerIssueMutations* Mutations() override;
    const std::vector<TrackerField>& AvailableFields() const override;
    RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                         const std::string& issueTypeName) const override;
    void LaunchBackgroundTask(std::function<void()> task) override;
    void RefreshLocalData() override;
    void RequestDeferredLiveTrackerBackendSuccessNotify() override;
    // Declared in BOTH interfaces (same signature) — this single override satisfies both,
    // like RequestDeferredLiveTrackerBackendSuccessNotify above. Forwards to the context's
    // mutex-guarded key (multi-grid Slice 1b).
    std::string CacheBackendKey() const override;

    // ---- ITicketSyncDeps --------------------------------------------------------------
    ITrackerIssueReader* Backend() override;
    ITrackerConnectivity* BackendConnectivity() override;
    void SetBackend(std::unique_ptr<ITrackerBackend> backend) override;
    ITrackerBackendFactory* BackendFactory() override;
    void SetCacheBackendKey(const std::string& key) override;
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
    AppController& app_;   ///< Shared/global state (cache, connectivity, catalog, Lua).
    GridLiveContext& ctx_; ///< Per-context state (backend slot, active-ticket snapshot).
};
