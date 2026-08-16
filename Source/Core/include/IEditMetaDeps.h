#pragma once

// IEditMetaDeps — narrow interface bundle exposing every AppController-side dependency that
// EditMetaCacheService reaches into while loading / pruning / warming per-issue + per-issue-type
// tracker edit metadata. Mirrors the ITicketSyncDeps / IOfflineQueueDeps template: AppController
// constructs `GridContextDepsAdapter` (which implements this interface alongside the two existing
// ones) and hands it to `EditMetaCacheService`. The service holds an `IEditMetaDeps&` reference
// and never touches AppController internals directly.
// Lifetime contract mirrors `ITicketSyncDeps`: the implementer (GridContextDepsAdapter) is owned
// by AppController and outlives the EditMetaCacheService. Background warm workers launched via
// `LaunchBackgroundTask` join inside `AppController::JoinBackgroundTasks` (called by
// ~AppController) before the adapter is destroyed.
// Test fixtures implement this interface directly (see tests/support/FakeEditMetaDeps.h) so unit
// tests can exercise EditMetaCacheService without constructing an AppController.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CachedTicketTypes.h" // for CachedTicket (in the shared_ptr<vector<CachedTicket>> return)

class ITrackerBackend;
struct TrackerField;
// GridContextFieldCatalog is defined in GridLiveContext.h (NOT its own header); forward-declared
// here so the interface does not pull GridLiveContext.h. The #975 kick-time pointer is threaded
// through as an opaque handle — only the service .cpp (which includes GridLiveContext.h) touches
// its members.
struct GridContextFieldCatalog;

class IEditMetaDeps {
  public:
    virtual ~IEditMetaDeps() = default;

    /// Latched backend handle (aliasing shared_ptr onto the atomic_load'ed per-context backend) —
    /// off-thread warm workers capture this so a live backend swap / context retirement can never
    /// dangle the backend mid-fetch (ADR-0012). Returns null when no backend is active.
    virtual std::shared_ptr<ITrackerBackend> BackendShared() const = 0;

    /// Spawn `task` on a tracked background thread (never detached) — joined in
    /// `JoinBackgroundTasks` before destruction. Same signature as `IOfflineQueueDeps`; the one
    /// adapter override satisfies both.
    virtual void LaunchBackgroundTask(std::function<void()> task) = 0;

    /// True once shutdown has begun — warm workers poll this to bail out of long loops.
    virtual bool IsShuttingDown() const = 0;

    /// Published active-tickets snapshot — used by `PruneEditMetaCacheToActiveTickets`,
    /// `ResolveIssueTypeKeyForIssue`, and the warm-start representative scan.
    virtual std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const = 0;

    /// Published roster snapshot of EVERY live grid pane. The editmeta cache is process-wide while
    /// `GetActiveTicketsSnapshot()` answers for one pane, so `PruneEditMetaCacheToActiveTickets`
    /// must retain the union — pruning to a single pane evicts entries another pane's open editor
    /// is still using, and every such miss costs a fresh editmeta round-trip. Defaulted to the
    /// single-pane snapshot so the test fakes and any single-context implementation keep working.
    virtual std::vector<std::shared_ptr<const std::vector<CachedTicket>>> GetActiveTicketsSnapshotsAllPanes() const {
        std::vector<std::shared_ptr<const std::vector<CachedTicket>>> one;
        one.push_back(GetActiveTicketsSnapshot());
        return one;
    }

    /// Catalog lookup for a field id (avoids re-scanning); used by `CanEditFieldForIssue` to
    /// detect sprint fields. Returns null when the field is unknown.
    virtual const TrackerField* FindFieldById(const std::string& fieldId) const = 0;

    /// Arm the deferred live-tracker-backend success notify (a successful editmeta fetch counts
    /// as a reachable-backend signal). CONST — a distinct overload from the existing non-const
    /// `RequestDeferredLiveTrackerBackendSuccessNotify()` on the adapter; both are needed.
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() const = 0;

    /// #975: the KICK-TIME per-context field catalog, captured on the UI thread when the warm
    /// worker is kicked. Returns `&ctx_.fieldCatalog`. The warm worker writes
    /// `projectComponentOptions_` / `projectComponentsInFlight_` / `projectComponentsRetryAfter_`
    /// under the catalog's OWN `availableFieldsMutex_` (a different mutex than the editmeta mutex)
    /// via this pointer — NEVER re-resolve the catalog at write time (that re-creates the
    /// completion-time-resolve leak #975 fixed).
    virtual GridContextFieldCatalog* KickTimeFieldCatalog() = 0;
};
