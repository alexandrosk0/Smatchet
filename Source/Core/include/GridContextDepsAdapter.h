#pragma once

// GridContextDepsAdapter — concrete implementation of `IOfflineQueueDeps` and
// `ITicketSyncDeps` that forwards per-context methods (`Backend`/`SetBackend`/`ActiveTickets*`)
// to a `GridLiveContext&` and global methods (`Cache`, connectivity banner, offline-replay
// timers, Lua notify) to a live `AppController&`. Split from the former
// AppControllerDepsAdapter in multi-grid Slice 1 (ADR-0018): the adapter is the chokepoint
// that lets `ITicketSyncDeps` — and therefore TicketSyncService, its tests, and every
// external call site — stay unchanged while the engine state de-singletons.
// TWO MODES (see the constructors): PANE-FROZEN latches one `GridLiveContext&` for life, and
// FOCUS-FOLLOWING re-resolves `app.focusedContext()` on every call. Every per-context method
// goes through the private `ctx()` accessor, which resolves whichever mode this adapter is in.
// Owned by AppController via `std::unique_ptr` (`depsAdapter_` for the focus-following one,
// `paneAdapters_[paneId]` for the pane-frozen ones); constructed in `AppController::Initialize`
// / `EnsurePaneContextLive` and destroyed before any AppController member it forwards to. A
// pane-frozen adapter never dereferences a dead `ctx_`: retired contexts park as ADR-0012 husks
// in `retiredContexts_` until `~AppController`, and the only caller of a pane-frozen adapter is
// that context's own TicketSyncService, which dies with it.
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
    /// PANE-FROZEN mode: every per-context method resolves to `ctx` for the adapter's whole life.
    /// Used for a pane's own `TicketSyncService` — the service must keep talking to the context it
    /// was created for even while another pane holds focus.
    GridContextDepsAdapter(AppController& app, GridLiveContext& ctx);

    /// FOCUS-FOLLOWING mode: every per-context method resolves to `app.focusedContext()` at CALL
    /// time. Used for the process-wide services AppController owns one of (edit-meta, field-edit,
    /// offline-queue, connectivity, attachment/app-update): they act on whatever pane the user is
    /// looking at, so freezing them to the default pane at Initialize time made a Plane-pane cell
    /// edit submit against the default pane's Jira backend + cache key.
    explicit GridContextDepsAdapter(AppController& app);

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
    /// Generation-checked variant (issue #1081): forwards THIS adapter's ctx() into
    /// AppController's checked refresh impl. On a pane-frozen adapter capture + re-check + apply
    /// all happen on the SAME context. On a focus-following adapter a focus switch mid-flight can
    /// re-resolve a DIFFERENT context here — that drops the apply rather than landing it on the
    /// wrong pane, because backendGeneration_ is seeded per context from a disjoint band
    /// (NextBackendGenerationSeed, GridLiveContext.h) so a cross-context compare never matches.
    /// Safe to call from replay workers: retired contexts park as defer-free husks in
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
    /// Sibling-pane retention set — forwards to AppController, which walks the other live
    /// contexts sharing this cache namespace (see ITicketSyncDeps for why).
    std::vector<std::string> TicketIdsRetainedByOtherContexts() const override;
    /// Records THIS pane's kept ids under (cache backend key, pane id) so a sibling pane's sweep
    /// can subtract them even after the hidden-pane LRU has evicted this context's rows.
    void PublishOwnedTicketIds(const std::vector<std::string>& ids) override;
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
    /// Every live pane's roster, for the process-wide editmeta prune (see IEditMetaDeps).
    std::vector<std::shared_ptr<const std::vector<CachedTicket>>> GetActiveTicketsSnapshotsAllPanes() const override;
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
    bool FocusedFieldCatalogErrorTransient() const override;
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
    /// The context every per-context method resolves against: `*ctx_` in pane-frozen mode,
    /// `app_.focusedContext()` (re-resolved per call) in focus-following mode. Defined in the .cpp
    /// because AppController is only forward-declared here.
    GridLiveContext& ctx() const;

    AppController& app_;   ///< Shared/global state (cache, connectivity, catalog, Lua).
    GridLiveContext* ctx_; ///< Frozen per-context state; null = follow the focused pane.
};
