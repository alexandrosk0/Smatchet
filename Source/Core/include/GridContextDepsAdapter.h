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
#include "IEditMetaDeps.h"
#include "IFieldEditDeps.h"
#include "IConnectivityDeps.h"
#include "IAttachmentAppUpdateDeps.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ITrackerBackend.h"

class AppController;
class ISyncCache;
class ITrackerConnectivity;
class ITrackerIssueMutations;
class ITrackerIssueReader;
class ITrackerBackendFactory;
struct GridLiveContext;
struct GridContextFieldCatalog;
struct HostCallbacks;
struct TrackerConfig;
struct TrackerField;

class GridContextDepsAdapter : public IOfflineQueueDeps,
                               public ITicketSyncDeps,
                               public IEditMetaDeps,
                               public IFieldEditDeps,
                               public IConnectivityDeps,
                               public IAttachmentAppUpdateDeps {
  public:
    GridContextDepsAdapter(AppController& app, GridLiveContext& ctx);

    // ---- IOfflineQueueDeps ------------------------------------------------------------
    ISyncCache* Cache() override;
    std::shared_ptr<ISyncCache> CacheShared() override;
    /// Latched role handles (aliasing shared_ptr onto the atomic_load'ed backend) — replay
    /// workers capture these so a live backend swap / Slice-3 context retirement can never
    /// dangle the subobject pointers (debt 2026-06-07).
    std::shared_ptr<ITrackerIssueReader> ReaderShared() const override;
    std::shared_ptr<ITrackerIssueMutations> MutationsShared() const override;
    const std::vector<TrackerField>& AvailableFields() const override;
    RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                         const std::string& issueTypeName) const override;
    void LaunchBackgroundTask(std::function<void()> task) override;
    void RefreshLocalData() override;
    /// Generation-checked variant (issue #1081): forwards THIS adapter's latched ctx_ into
    /// AppController's checked refresh impl so capture + re-check + apply all happen on the
    /// SAME context (a focused-context re-resolve at apply time could compare another pane's
    /// independent generation counter — equal-by-coincidence passes the gate). Safe to call
    /// from replay workers: retired contexts park as defer-free husks in
    /// AppController::retiredContexts_ until ~AppController, so ctx_ never dangles.
    void RefreshLocalData(std::uint64_t capturedBackendGeneration) override;
    void RequestDeferredLiveTrackerBackendSuccessNotify() override;
    // Declared in BOTH interfaces (same signature) — this single override satisfies both,
    // like RequestDeferredLiveTrackerBackendSuccessNotify above. Forwards to the context's
    // mutex-guarded key (multi-grid Slice 1b).
    std::string CacheBackendKey() const override;
    /// Capture-then-check token (issue #1081) — the context's backendGeneration_ atomic.
    std::uint64_t BackendGeneration() const override;

    // ---- ITicketSyncDeps --------------------------------------------------------------
    ITrackerIssueReader* Backend() override;
    ITrackerConnectivity* BackendConnectivity() override;
    void SetBackend(std::unique_ptr<ITrackerBackend> backend) override;
    ITrackerBackendFactory* BackendFactory() override;
    void SetCacheBackendKey(const std::string& key) override;
    void SetLastTrackerTicketSyncWarning(const std::string& message, bool transient) override;
    void SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState state) override;
    void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) override;
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) override;
    void NotifySyncStatus(const std::string& title, const std::string& message, ITicketSyncDeps::SyncNotifyLevel level,
                          int durationMs) override;
    // Failed session → re-arm the context's initial-sync latch + drop its recorded JQL so
    // the next pane focus switch retries instead of suppressing the re-sync forever
    // UI thread (TickStreamingApply) — same single-thread
    // discipline as the latch itself.
    void OnStreamingSyncSessionFinished(bool fetchOk) override;
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

    // ---- IEditMetaDeps ----------------------------------------------------------------
    // LaunchBackgroundTask is shared with IOfflineQueueDeps (same signature) — the override above
    // satisfies this interface's method too.
    std::shared_ptr<ITrackerBackend> BackendShared() const override;
    bool IsShuttingDown() const override;
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override;
    const TrackerField* FindFieldById(const std::string& fieldId) const override;
    // CONST overload — distinct from the non-const RequestDeferredLiveTrackerBackendSuccessNotify()
    // above (shared by IOfflineQueueDeps + ITicketSyncDeps). Both are needed; they forward to the
    // same AppController method.
    void RequestDeferredLiveTrackerBackendSuccessNotify() const override;
    // #975: hand back THIS context's kick-time catalog so the warm worker writes the per-project
    // component options under the catalog's own lock without a completion-time re-resolve.
    GridContextFieldCatalog* KickTimeFieldCatalog() override;

    // ---- IFieldEditDeps ---------------------------------------------------------------
    // BackendShared() / GetActiveTicketsSnapshot() (IEditMetaDeps), RefreshLocalData()
    // (IOfflineQueueDeps), and the CONST RequestDeferredLiveTrackerBackendSuccessNotify()
    // (IEditMetaDeps) are all already declared above — the single override of each satisfies
    // IFieldEditDeps too (do NOT redeclare them). Only these two are genuinely new:
    bool HasCache() const override;
    void UpdateTicket(const CachedTicket& ticket) override;

    // ---- IConnectivityDeps ------------------------------------------------------------
    // IsShuttingDown() is shared with IEditMetaDeps (same signature) — the override above
    // satisfies this interface too. These accessors are deliberately DISTINCT from the frozen-ctx_
    // BackendShared() / catalog overrides above: the connectivity FSM re-resolves the FOCUSED
    // context LIVE on every call, so these forward to app_.BackendShared() / app_.fieldCatalog()
    // (LIVE focus), NOT ctx_. Reusing the frozen-ctx_ bodies would silently break multi-pane
    // behaviour after a focus switch (Phase 3 R1). Catalog reads/clears stay UNLOCKED, preserving
    // the UI-thread-only single-kick-time-latch discipline (no availableFieldsMutex_).
    std::shared_ptr<ITrackerBackend> FocusedBackendShared() const override;
    const std::string& FocusedFieldCatalogError() const override;
    const std::string& FocusedFieldCatalogWarning() const override;
    void ClearFocusedFieldCatalogWarning() override;
    void BumpFocusedFieldCatalogRevision() override;
    void PushReplayTimers(std::chrono::steady_clock::time_point pushTo) override;
    void RestartReplayTimers(std::chrono::steady_clock::time_point now) override;

    // ---- IAttachmentAppUpdateDeps -----------------------------------------------------
    // All three are genuinely-new signatures (no sibling interface declares them): the host-callback
    // struct read + the const OpenUrl / RequestAppQuit forwards the attachment + installer paths use.
    const HostCallbacks& Host() const override;
    void OpenUrl(const std::string& url) const override;
    void RequestAppQuit() const override;

  private:
    AppController& app_;   ///< Shared/global state (cache, connectivity, catalog, Lua).
    GridLiveContext& ctx_; ///< Per-context state (backend slot, active-ticket snapshot).
};
