#pragma once

// ITicketSyncDeps — interface bundle exposing every AppController-side dependency that
// TicketSyncService reaches into during streaming-sync, batch apply, and stale-deletion.
// Background: TicketSyncService was extracted from AppController via `friend` access during
// the item 11 migration (see TicketSyncService.h header comment). This interface removes the
// `friend` seam: AppController constructs `GridContextDepsAdapter` (which implements this
// interface alongside `IOfflineQueueDeps`) and hands it to `TicketSyncService`. The service no
// longer holds an `AppController&` reference and therefore no longer needs trusted-friendship
// access.
// Lifetime contract mirrors `IOfflineQueueDeps`: the implementer (GridContextDepsAdapter) is
// owned by AppController and outlives the TicketSyncService. The streaming-sync worker thread
// joins inside `TicketSyncService::CancelAndJoinActiveStreamingSync`, which `~AppController`
// invokes before the adapter is destroyed.
// Test fixtures implement this interface directly (see tests/support/FakeTicketSyncDeps.h) so
// unit tests can exercise TicketSyncService without constructing an AppController.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CachedTicketTypes.h" // for CachedTicket (in vector & shared_ptr returns)

class ISyncCache;
class ITrackerBackend;
class ITrackerIssueReader;
class ITrackerConnectivity;
class ITrackerBackendFactory;
struct TrackerConfig;

class ITicketSyncDeps {
  public:
    /// Mirrors `AppController::TrackerConnectivityState`. Declared here so the deps interface
    /// does not pull AppController.h. The integer values must stay in lockstep with the
    /// AppController enum — `GridContextDepsAdapter::SetLastTrackerConnectivityState` casts
    /// between them. The corresponding doctest fixture exercises the same enum.
    enum class ConnectivityState {
        Unknown = 0,
        AuthenticatedReachable = 1,
        ReachableAuthOrConfigError = 2,
        TransportDown = 3,
        ServiceUnavailable = 4,
    };

    /// Severity of a user-facing sync notification. UI-free analogue of `ToastType` (the deps
    /// adapter maps it to a toast in the GUI build) so the interface does not pull `imgui.h`
    /// via `SmatchetToast.h`. Decouples TicketSyncService from the ImGui toast layer
    /// (debt 2026-06-07 — sync-imgui-coupling).
    enum class SyncNotifyLevel {
        Info,
        Success,
        Warning,
        Error,
    };

    virtual ~ITicketSyncDeps() = default;

    // ---- Cache + backend handles ------------------------------------------------------
    virtual ISyncCache* Cache() = 0;
    /// DR6 latched cache snapshot — strong handle over the same cache Cache() returns, so an
    /// off-thread caller holds it alive across its use while the UI thread swaps the cache in
    /// RecreateLocalCacheDatabase. Mirrors the ADR-0012 Backend atomic-swap pattern. May be null.
    virtual std::shared_ptr<ISyncCache> CacheShared() = 0;
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
    /// Backend-key namespace for every LocalCacheManager ticket read/write (multi-grid Slice 1b,
    /// ADR-0018 decision 4). Returns a copy — callable from worker threads while the UI thread
    /// rewrites the key on a tracker swap. Shared (same override) with `IOfflineQueueDeps`.
    virtual std::string CacheBackendKey() const = 0;
    /// Re-stamp the namespace after a backend-kind swap (`SwapBackendIfTrackerChanged`). The
    /// value MUST be `ConfigManager::NormalizeViewsBackendKey` output.
    virtual void SetCacheBackendKey(const std::string& key) = 0;
    /// Ticket ids currently held by the OTHER live grid panes sharing this cache namespace.
    /// A full sync marks every cache row its OWN query did not return as stale, which is only
    /// correct when that query covers the whole namespace. It does not: the SQLite cache is
    /// backend-scoped and shared (ADR-0018 decision 4) while each pane syncs its own JQL, so
    /// without this subtraction two differently-scoped panes delete each other's rows every
    /// cycle and both grids end up empty. Stale-deletion subtracts these ids before deleting.
    /// UI thread only, once per completed full-sync session — see the lock-order note at the
    /// call site. Defaulted empty so single-context deps implementations (the test fakes) keep
    /// the previous behaviour without a stub.
    virtual std::vector<std::string> TicketIdsRetainedByOtherContexts() const { return {}; }

    // ---- Connectivity + sync-warning banner -------------------------------------------
    /// `transient` = the warning stems from a transport-shaped failure (offline / DNS / timeout /
    /// 5xx-style), classified at the fetch seam that composed it (N12 slice 1). Consumers (the
    /// connectivity monitor's degraded-probe-interval check) branch on the flag instead of
    /// re-classifying the composed message text.
    virtual void SetLastTrackerTicketSyncWarning(const std::string& message, bool transient) = 0;
    virtual void SetLastTrackerConnectivityState(ConnectivityState state) = 0;
    virtual void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) = 0;
    /// Forward to `OfflineQueueService::PushReplayTimersForward`. AppController routes this so
    /// the two services don't need direct knowledge of each other.
    virtual void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) = 0;
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() = 0;
    /// Surface a transient user-facing sync notification (a toast in the GUI build; a no-op /
    /// recorded event in headless + test builds). Routed through the deps adapter so
    /// TicketSyncService carries no UI / ImGui dependency. `durationMs` is the on-screen dwell.
    virtual void NotifySyncStatus(const std::string& title, const std::string& message, SyncNotifyLevel level,
                                  int durationMs) = 0;
    /// Session-end notification (UI thread — fired once per streaming session from
    /// `FinalizeStreamingSessionIfDone`, after the error/warning classification).
    /// `fetchOk` is false when the session ended with a non-empty fetch error.
    /// GridContextDepsAdapter uses a failed end to re-arm the context's initial-sync
    /// latch + drop its recorded JQL so the next pane focus switch retries instead of
    /// adopting the failed result forever (review MEDIUM-1). Defaulted no-op so test
    /// fixtures and future deps implementers without per-pane latches need no stub.
    virtual void OnStreamingSyncSessionFinished(bool fetchOk) { (void)fetchOk; }

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
