#include "GridContextDepsAdapter.h"

#include "AppController.h"
#include "ConnectivityMonitorService.h"
#include "GridLiveContext.h"
#include "Sync/OfflineQueueService.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "LocalCacheManager.h"
#include "SmatchetToast.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

GridContextDepsAdapter::GridContextDepsAdapter(AppController& app, GridLiveContext& ctx) : app_(app), ctx_(&ctx) {}

GridContextDepsAdapter::GridContextDepsAdapter(AppController& app) : app_(app), ctx_(nullptr) {}

// Pane-frozen adapters answer with their latched context; focus-following adapters re-resolve the
// focused pane on EVERY call. focusedContext() is never null (the default context is permanent) and
// returns a non-const reference through app_ — constness does not propagate through a reference
// member, so this stays callable from the const overrides below.
GridLiveContext& GridContextDepsAdapter::ctx() const { return ctx_ != nullptr ? *ctx_ : app_.focusedContext(); }

// ---- IOfflineQueueDeps (and shared with ITicketSyncDeps) ------------------------------

ISyncCache* GridContextDepsAdapter::Cache() { return app_.Cache.get(); }

// DR6: atomic_load snapshot of the cache shared_ptr so an off-thread caller (QueueCreateOffline)
// holds it alive across the whole operation while the UI thread swaps it in
// RecreateLocalCacheDatabase. Mirrors the ADR-0012 Backend atomic_load reader pattern above.
// LocalCacheManager derives from ISyncCache, so the shared_ptr converts implicitly.
std::shared_ptr<ISyncCache> GridContextDepsAdapter::CacheShared() { return std::atomic_load(&app_.Cache); }

// Backend reads go through std::atomic_load so the shared_ptr-instance read can't race the
// live swap (atomic_store/atomic_exchange in SetBackend). The ALIASING shared_ptr keeps the
// whole backend alive for as long as the caller holds the role handle — stronger than the
// ADR-0012 graveyard alone, which only covers swaps, not Slice-3 context retirement.
std::shared_ptr<ITrackerIssueReader> GridContextDepsAdapter::ReaderShared() const {
    std::shared_ptr<ITrackerBackend> b = std::atomic_load(&ctx().Backend);
    if (!b) {
        return nullptr;
    }
    return std::shared_ptr<ITrackerIssueReader>(b, &b->Reader());
}

std::shared_ptr<ITrackerIssueMutations> GridContextDepsAdapter::MutationsShared() const {
    std::shared_ptr<ITrackerBackend> b = std::atomic_load(&ctx().Backend);
    if (!b || b->Mutations() == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<ITrackerIssueMutations>(b, b->Mutations());
}

ITrackerConnectivity* GridContextDepsAdapter::BackendConnectivity() {
    auto b = std::atomic_load(&ctx().Backend);
    return b ? &b->Connectivity() : nullptr;
}

// Per-context (multi-grid Slice 3): the catalog moved into GridLiveContext, so replay /
// sync paths read THIS context's catalog — not whichever pane happens to be focused.
const std::vector<TrackerField>& GridContextDepsAdapter::AvailableFields() const {
    return ctx().fieldCatalog.AvailableFields;
}

RequiredFieldSet GridContextDepsAdapter::GetRequiredFieldSet(const std::string& projectKey,
                                                             const std::string& issueTypeId,
                                                             const std::string& issueTypeName) const {
    return app_.GetRequiredFieldSet(projectKey, issueTypeId, issueTypeName);
}

void GridContextDepsAdapter::LaunchBackgroundTask(std::function<void()> task) {
    app_.LaunchBackgroundTask(std::move(task));
}

void GridContextDepsAdapter::RefreshLocalData() { app_.RefreshLocalData(); }

// Friend access: the checked impl is private on purpose — every checked caller must name the
// context it latched the generation from, and this adapter's ctx() is exactly that context.
// Focus-following adapters can resolve a DIFFERENT context here than the one BackendGeneration()
// was captured from (a focus switch mid-replay). That is safe rather than silently wrong because
// backendGeneration_ is seeded per context from a process-wide stride (GridLiveContext.h), so a
// cross-context comparison always mismatches and the apply is dropped instead of landing on the
// wrong pane.
void GridContextDepsAdapter::RefreshLocalData(std::uint64_t capturedBackendGeneration) {
    app_.RefreshLocalDataCheckedImpl_(ctx(), &capturedBackendGeneration);
}

void GridContextDepsAdapter::RequestDeferredLiveTrackerBackendSuccessNotify() {
    app_.requestDeferredLiveTrackerBackendSuccessNotify_();
}

// Declared in both IOfflineQueueDeps and ITicketSyncDeps; this single override satisfies both.
std::string GridContextDepsAdapter::CacheBackendKey() const { return ctx().CacheBackendKeyCopy(); }

std::uint64_t GridContextDepsAdapter::BackendGeneration() const { return ctx().backendGeneration_.load(); }

// ---- ITicketSyncDeps ------------------------------------------------------------------

ITrackerIssueReader* GridContextDepsAdapter::Backend() {
    auto b = std::atomic_load(&ctx().Backend);
    return b ? &b->Reader() : nullptr;
}

void GridContextDepsAdapter::SetBackend(std::unique_ptr<ITrackerBackend> backend) {
    // Live tracker swap. atomic_exchange swaps the slot AND returns the old backend in one
    // synchronised step — off-thread workers read ctx().Backend via std::atomic_load, and a plain
    // assignment would data-race the shared_ptr instance in C++14). The old backend is then
    // RETIRED (defer-free) into the AppController-shared graveyard, not freed, so any raw
    // Reader/Mutations/Connectivity pointer a worker captured before the swap stays valid
    // until shutdown. See ADR 0012.
    std::shared_ptr<ITrackerBackend> incoming(std::move(backend));
    std::shared_ptr<ITrackerBackend> old = std::atomic_exchange(&ctx().Backend, incoming);
    // Invalidate in-flight work captured against the OLD backend (issue #1081): workers
    // capture backendGeneration_ at work-capture time and drop their apply on mismatch
    // (capture-then-check — see GridLiveContext.h).
    ctx().backendGeneration_.fetch_add(1);
    app_.RetireBackend(std::move(old));
}

ITrackerBackendFactory* GridContextDepsAdapter::BackendFactory() { return app_.backendFactory_.get(); }

void GridContextDepsAdapter::SetCacheBackendKey(const std::string& key) { ctx().SetCacheBackendKey(key); }

std::vector<std::string> GridContextDepsAdapter::TicketIdsRetainedByOtherContexts() const {
    return app_.CollectTicketIdsRetainedByOtherContexts(ctx());
}

void GridContextDepsAdapter::PublishOwnedTicketIds(const std::vector<std::string>& ids) {
    GridLiveContext& c = ctx();
    app_.SetPaneOwnedTicketIds(c.CacheBackendKeyCopy(), c.PaneId, ids);
}

// Re-pointed (Phase 3): the connectivity state moved into the GLOBAL ConnectivityMonitorService.
// Every per-pane adapter forwards its connectivity writes into that one instance (N writers, one
// service). The service is constructed eagerly in Initialize before the first tick, so these
// runtime overrides always have a live target.
void GridContextDepsAdapter::SetLastTrackerTicketSyncWarning(const std::string& message, bool transient) {
    if (app_.connectivity_) {
        app_.connectivity_->SetLastTicketSyncWarning(message, transient);
    }
}

void GridContextDepsAdapter::SetLastTrackerConnectivityState(ITicketSyncDeps::ConnectivityState state) {
    if (app_.connectivity_) {
        // Lockstep cast (Phase 3 R5): the deps-side connectivity-state enum mirrors the global
        // tracker connectivity-state enum, with values kept in sync and asserted by the fixture.
        app_.connectivity_->SetLastState(static_cast<::TrackerConnectivityState>(state));
    }
}

void GridContextDepsAdapter::SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) {
    if (app_.connectivity_) {
        app_.connectivity_->SetNextProbeAt(at);
    }
}

void GridContextDepsAdapter::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    app_.PushOfflineReplayTimersDuringTransportOutage(now);
}

// UI-glue: map the UI-free SyncNotifyLevel onto a ToastType and push a toast. This adapter is
// the chokepoint where TicketSyncService's UI-free notification re-acquires its ImGui toast
// rendering (debt 2026-06-07 — sync-imgui-coupling). No `default:` case so a future enumerator
// trips Clang -Werror,-Wswitch instead of silently mapping to Info.
void GridContextDepsAdapter::NotifySyncStatus(const std::string& title, const std::string& message,
                                              ITicketSyncDeps::SyncNotifyLevel level, int durationMs) {
    ToastType type = ToastType::Info;
    switch (level) {
    case ITicketSyncDeps::SyncNotifyLevel::Info:
        type = ToastType::Info;
        break;
    case ITicketSyncDeps::SyncNotifyLevel::Success:
        type = ToastType::Success;
        break;
    case ITicketSyncDeps::SyncNotifyLevel::Warning:
        type = ToastType::Warning;
        break;
    case ITicketSyncDeps::SyncNotifyLevel::Error:
        type = ToastType::Error;
        break;
    }
    SmatchetToastManager::Instance().Push(title, message, type, durationMs);
}

void GridContextDepsAdapter::OnStreamingSyncSessionFinished(bool fetchOk) {
    if (!fetchOk) {
        // Failed session: re-arm so the next focus switch / visibility kick retries the
        // sync instead of treating the pane as sync-live forever.
        // UI thread — same single-thread discipline as the latch.
        ctx().initialSyncKicked = false;
        ctx().lastSyncedJql.clear();
        // Storm damping (issue #1081): the re-arm above + the per-frame kick site retried a
        // fast-failing backend at FRAME RATE. Open a 30 s retry window instead — the kick
        // site bails while now < syncRetryAfter (PaneSyncKickPolicy.h; mirrors the
        // projectComponentsRetryAfter_ backoff). UI thread — same discipline as the latch.
        ctx().syncRetryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
}

std::mutex& GridContextDepsAdapter::ActiveTicketsMutex() { return ctx().activeTicketsMutex_; }

std::vector<CachedTicket>& GridContextDepsAdapter::ActiveTickets() { return ctx().ActiveTickets; }

void GridContextDepsAdapter::SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) {
    // CONTRACT (issue #1081): the caller MUST hold ctx().activeTicketsMutex_ —
    // activeTicketsPublished_ is guarded by it (GridLiveContext.h). An unguarded publish
    // races worker-thread writers and locked readers on the shared_ptr control block
    // (C++14 UB → std::terminate). Enforced by the publish-under-lock doctest case
    // (BackendSwitchRace1081.test.cpp); std::mutex has no portable same-thread "is held
    // by me" probe, so a runtime assert here would itself be UB — keep new call sites
    // inside an ActiveTicketsMutex() lock scope.
    ctx().activeTicketsPublished_ = std::move(snap);
}

void GridContextDepsAdapter::BumpActiveTicketsRevision() { ctx().ActiveTicketsRevision.fetch_add(1); }

void GridContextDepsAdapter::PruneEditMetaCacheToActiveTickets() { app_.PruneEditMetaCacheToActiveTickets(); }

void GridContextDepsAdapter::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig cfg) {
    app_.WarmIssueTypeEditMetaAtStartAsync(std::move(cfg));
}

void GridContextDepsAdapter::NotifyLuaTicketDataChanged() { app_.NotifyLuaTicketDataChanged(); }

bool GridContextDepsAdapter::GetPendingLuaWindowBump() const { return app_.pendingLuaWindowBump_; }

void GridContextDepsAdapter::SetPendingLuaWindowBump(bool value) { app_.pendingLuaWindowBump_ = value; }

// ---- IEditMetaDeps --------------------------------------------------------------------

// Per-context latched backend (matches the ReaderShared/Backend discipline above): atomic_load so
// the shared_ptr-instance read can't race a live SetBackend swap. The whole backend stays alive as
// long as the warm worker holds the returned shared_ptr (ADR-0012).
std::shared_ptr<ITrackerBackend> GridContextDepsAdapter::BackendShared() const {
    return std::atomic_load(&ctx().Backend);
}

bool GridContextDepsAdapter::IsShuttingDown() const { return app_.IsShuttingDown(); }

// THIS adapter's context, not AppController::GetActiveTicketsSnapshot() (which always re-resolves
// the focused pane). A pane-frozen adapter must answer with its OWN pane's rows — otherwise an
// edit-meta / field-edit call raised by a background pane validated against the focused pane's grid.
std::shared_ptr<const std::vector<CachedTicket>> GridContextDepsAdapter::GetActiveTicketsSnapshot() const {
    return ctx().ActiveTicketsSnapshot();
}

std::vector<std::shared_ptr<const std::vector<CachedTicket>>>
GridContextDepsAdapter::GetActiveTicketsSnapshotsAllPanes() const {
    return app_.CollectActiveTicketSnapshotsAcrossContexts();
}

const TrackerField* GridContextDepsAdapter::FindFieldById(const std::string& fieldId) const {
    return app_.FindFieldById(fieldId);
}

// CONST overload, distinct from the non-const RequestDeferredLiveTrackerBackendSuccessNotify above —
// both forward to the same AppController method (which is itself const).
void GridContextDepsAdapter::RequestDeferredLiveTrackerBackendSuccessNotify() const {
    app_.requestDeferredLiveTrackerBackendSuccessNotify_();
}

// #975: THIS context's catalog, captured on the UI thread at kick time. The warm worker writes
// projectComponentOptions_/InFlight_/RetryAfter_ under cat.availableFieldsMutex_ via this pointer —
// never a completion-time fieldCatalog() re-resolve.
GridContextFieldCatalog* GridContextDepsAdapter::KickTimeFieldCatalog() { return &ctx().fieldCatalog; }

// ---- IFieldEditDeps -------------------------------------------------------------------
// BackendShared / GetActiveTicketsSnapshot / RefreshLocalData / const-RequestDeferred are reused
// from the IEditMetaDeps + IOfflineQueueDeps overrides above. Only these two are genuinely new.

// HasCache() is a PREDICATE, not the raw Cache* — keeps FieldEditPipelineService SQLite-free
// (ADR-0020 sync-cache purity). Global state, so reads app_ (matches Cache() above).
bool GridContextDepsAdapter::HasCache() const { return app_.Cache != nullptr; }

void GridContextDepsAdapter::UpdateTicket(const CachedTicket& ticket) { app_.UpdateTicket(ticket); }

// ---- IConnectivityDeps ----------------------------------------------------------------
// DISTINCT from the ctx()-routed BackendShared() / catalog accessors above (Phase 3 R1): the
// connectivity FSM re-resolves the FOCUSED context LIVE every call, so these forward to
// app_.BackendShared() / app_.fieldCatalog() (focused-context routing), NOT ctx(). IsShuttingDown()
// is reused from the IEditMetaDeps override above. Catalog reads/clears stay UNLOCKED — preserving
// the UI-thread-only single-kick-time-latch discipline (no availableFieldsMutex_).

std::shared_ptr<ITrackerBackend> GridContextDepsAdapter::FocusedBackendShared() const { return app_.BackendShared(); }

const std::string& GridContextDepsAdapter::FocusedFieldCatalogError() const {
    return app_.fieldCatalog().LastTrackerFieldCatalogError;
}

bool GridContextDepsAdapter::FocusedFieldCatalogErrorTransient() const {
    return app_.fieldCatalog().LastTrackerFieldCatalogErrorTransient;
}

const std::string& GridContextDepsAdapter::FocusedFieldCatalogWarning() const {
    return app_.fieldCatalog().LastTrackerFieldCatalogWarning;
}

void GridContextDepsAdapter::ClearFocusedFieldCatalogWarning() {
    app_.fieldCatalog().LastTrackerFieldCatalogWarning.clear();
}

void GridContextDepsAdapter::BumpFocusedFieldCatalogRevision() {
    app_.fieldCatalog().TrackerFieldCatalogRevision.fetch_add(1);
}

// Null-queue guard lives here (moved off the service per the plan) so the service stays unaware of
// offlineQueue_ lifetime. Forwards to OfflineQueueService's replay-timer controls.
void GridContextDepsAdapter::PushReplayTimers(std::chrono::steady_clock::time_point pushTo) {
    if (app_.offlineQueue_) {
        app_.offlineQueue_->PushReplayTimersForward(pushTo);
    }
}

void GridContextDepsAdapter::RestartReplayTimers(std::chrono::steady_clock::time_point now) {
    if (app_.offlineQueue_) {
        app_.offlineQueue_->RestartReplayTimersNow(now);
    }
}

// ---- IAttachmentAppUpdateDeps (Phase 4) ----------------------------------------------
// Global host state — these forward to app_ regardless of the adapter's mode (the host callbacks + URL-open +
// app-quit are AppController-global, not per-pane). The adapter is a friend of AppController, so the
// HostCallbacks struct read is direct. OpenUrl / RequestAppQuit are the existing public const methods
// (scheme-allowlist + null-handler guard live inside them).
const HostCallbacks& GridContextDepsAdapter::Host() const { return app_.hostCallbacks_; }

void GridContextDepsAdapter::OpenUrl(const std::string& url) const { app_.OpenUrl(url); }

void GridContextDepsAdapter::RequestAppQuit() const { app_.RequestAppQuit(); }
