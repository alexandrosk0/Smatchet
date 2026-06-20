#pragma once

// IFieldEditDeps — narrow interface bundle exposing every AppController-side dependency that
// FieldEditPipelineService reaches into while running the field-edit network pipeline
// (SubmitFieldEdit / SubmitFieldEditNetworkOnly / TryPrepareOfflineFieldEdit / ApplyFieldEditResult
// + their branch helpers). Mirrors the IEditMetaDeps / ITicketSyncDeps / IOfflineQueueDeps
// template: AppController constructs `GridContextDepsAdapter` (which implements this interface
// alongside the existing three) and hands it to `FieldEditPipelineService`. The service holds an
// `IFieldEditDeps&` reference and never touches AppController internals directly.
// Deliberately narrow (AppController god-object decomposition Phase 2): mutations are reached via
// BackendShared then Mutations so there is no MutationsShared here; the field is always supplied by
// the caller as a const TrackerField reference so there is no FindFieldById; and the offline-queue
// DB write is grid-layer (SmatchetGridFieldEditPipeline calls AppController QueueFieldEditOffline
// after the worker returns QueuedOffline), so this service only builds the payload via
// TryPrepareOfflineFieldEdit and exposes no QueueFieldEditOffline. Edit-metadata operations
// (EnsureIssueEditMetaLoaded, CanEditFieldForIssue, RefreshIssueEditMeta) are reached through the
// EditMetaCacheService reference the service holds directly (ctor-injected), not through this
// interface.
// Lifetime contract mirrors IEditMetaDeps: the implementer (GridContextDepsAdapter) is owned by
// AppController and outlives the FieldEditPipelineService. The pipeline holds no long-lived state
// across call frames (the SubmitFieldEditCtx is call-frame-scoped), so no background-task /
// shutdown hooks are needed here.
// Test fixtures implement this interface directly (see tests/support/FakeFieldEditDeps.h) so unit
// tests can exercise FieldEditPipelineService without constructing an AppController.

#include <memory>
#include <vector>

#include "CachedTicketTypes.h" // for CachedTicket (in the shared_ptr<vector<CachedTicket>> return + UpdateTicket arg)

class ITrackerBackend;

class IFieldEditDeps {
  public:
    virtual ~IFieldEditDeps() = default;

    /// Latched backend handle (aliasing shared_ptr onto the atomic_load'ed per-context backend) —
    /// the field-edit pipeline captures this once per call so a live backend swap / context
    /// retirement can never dangle the backend mid-edit (ADR-0012). Returns null when no backend
    /// is active. Mutations are reached via `BackendShared()->Mutations()`.
    virtual std::shared_ptr<ITrackerBackend> BackendShared() const = 0;

    /// True when the local cache is initialized. A PREDICATE, not the raw `Cache*` — keeps the
    /// service SQLite-free (ADR-0020 sync-cache purity): the pipeline only needs to know whether a
    /// cache exists before applying an optimistic local update.
    virtual bool HasCache() const = 0;

    /// Published active-tickets snapshot — used to find the edited ticket for the optimistic
    /// local-cache update + audit before/after values.
    virtual std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const = 0;

    /// Apply an optimistic local update for the edited ticket (SaveTicket + grid refresh). Called
    /// after a successful backend mutation so the grid reflects the edit without a full re-sync.
    virtual void UpdateTicket(const CachedTicket& ticket) = 0;

    /// Re-read the local cache into the active-tickets model + refresh the grid. Used when the
    /// edited ticket is not in the current snapshot. Declared on IOfflineQueueDeps too (same
    /// signature) — the single adapter override satisfies both.
    virtual void RefreshLocalData() = 0;

    /// Arm the deferred live-tracker-backend success notify (a successful field edit counts as a
    /// reachable-backend signal). CONST — the SAME overload the existing const
    /// IEditMetaDeps::RequestDeferredLiveTrackerBackendSuccessNotify() declares; the one adapter
    /// override satisfies both interfaces.
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() const = 0;
};
