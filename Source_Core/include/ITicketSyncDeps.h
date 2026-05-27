#pragma once

// ITicketSyncDeps — interface bundle exposing every AppController-side dependency that
// TicketSyncService reaches into during streaming-sync, batch apply, and stale-deletion.
//
// Background: TicketSyncService was extracted from AppController via `friend` access during
// the item 11 migration (see TicketSyncService.h header comment). This interface removes the
// `friend` seam: AppController constructs `AppControllerDepsAdapter` (which implements this
// interface alongside `IOfflineQueueDeps`) and hands it to `TicketSyncService`. The service no
// longer holds an `AppController&` reference and therefore no longer needs trusted-friendship
// access.
//
// Lifetime contract mirrors `IOfflineQueueDeps`: the implementer (AppControllerDepsAdapter) is
// owned by AppController and outlives the TicketSyncService. The streaming-sync worker thread
// joins inside `TicketSyncService::CancelAndJoinActiveStreamingSync`, which `~AppController`
// invokes before the adapter is destroyed.
//
// Test fixtures implement this interface directly (see tests/support/FakeTicketSyncDeps.h) so
// unit tests can exercise TicketSyncService without constructing an AppController.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "LocalCacheManager.h" // for CachedTicket (in vector & shared_ptr returns)

class ITrackerBackend;
class ITrackerIssueReader;
class ITrackerConnectivity;
class ITrackerBackendFactory;
struct TrackerConfig;

class ITicketSyncDeps {
  public:
    /// Mirrors `AppController::TrackerConnectivityState`. Declared here so the deps interface
    /// does not pull AppController.h. The integer values must stay in lockstep with the
    /// AppController enum — `AppControllerDepsAdapter::SetLastTrackerConnectivityState` casts
    /// between them. The corresponding doctest fixture exercises the same enum.
    enum class ConnectivityState {
        Unknown = 0,
        AuthenticatedReachable = 1,
        ReachableAuthOrConfigError = 2,
        TransportDown = 3,
        ServiceUnavailable = 4,
    };

    virtual ~ITicketSyncDeps() = default;

    // ---- Cache + backend handles ------------------------------------------------------
    virtual LocalCacheManager* Cache() = 0;
    virtual ITrackerIssueReader* Backend() = 0;
    /// Narrow connectivity view of the same backend — used for tracker-type detection during
    /// backend-swap logic. Returns null when no backend is active.
    virtual ITrackerConnectivity* BackendConnectivity() = 0;
    /// Swap the active backend (e.g. when the user changes tracker type from Jira to Plane).
    /// Takes ownership; passing `nullptr` clears the backend.
    virtual void SetBackend(std::unique_ptr<ITrackerBackend> backend) = 0;
    /// Factory for creating fresh `ITrackerBackend` instances when the tracker type changes.
    /// May be null if `Initialize` has not yet wired the default factory.
    virtual ITrackerBackendFactory* BackendFactory() = 0;

    // ---- Connectivity + sync-warning banner -------------------------------------------
    virtual void SetLastTrackerTicketSyncWarning(const std::string& message) = 0;
    virtual void SetLastTrackerConnectivityState(ConnectivityState state) = 0;
    virtual void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) = 0;
    /// Forward to `OfflineQueueService::PushReplayTimersForward`. AppController routes this so
    /// the two services don't need direct knowledge of each other.
    virtual void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) = 0;
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() = 0;

    // ---- Active-tickets cache ---------------------------------------------------------
    virtual std::mutex& ActiveTicketsMutex() = 0;
    virtual std::vector<CachedTicket>& ActiveTickets() = 0;
    /// Republish the cached `shared_ptr<const vector<CachedTicket>>` after `ActiveTickets`
    /// has been mutated under `ActiveTicketsMutex`. The caller already holds the mutex.
    virtual void SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) = 0;
    /// Bump `ActiveTicketsRevision.fetch_add(1)` — UI sort caches re-evaluate on bump.
    virtual void BumpActiveTicketsRevision() = 0;

    // ---- Edit-meta + Lua glue ---------------------------------------------------------
    virtual void PruneEditMetaCacheToActiveTickets() = 0;
    virtual void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig cfg) = 0;
    virtual void NotifyLuaTicketDataChanged() = 0;
    /// Set / read the coalesced fetch-session window-dirty flag. TicketSyncService flips this
    /// across many ApplyIssueFetchPack + stale-deletion + streaming-batch scopes, then fires
    /// `NotifyLuaTicketDataChanged` once at session end.
    virtual bool GetPendingLuaWindowBump() const = 0;
    virtual void SetPendingLuaWindowBump(bool value) = 0;
};
