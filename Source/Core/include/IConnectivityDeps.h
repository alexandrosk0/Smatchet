#pragma once

// IConnectivityDeps — narrow interface bundle exposing every AppController-side dependency that
// ConnectivityMonitorService reaches into while running the tracker connectivity-probe FSM
// (rate-limited reachability probe, recovery latch, the per-frame offline banner). Mirrors the
// ITicketSyncDeps / IEditMetaDeps / IFieldEditDeps template: AppController constructs
// GridContextDepsAdapter (which implements this interface alongside the four existing ones) and
// hands it to ConnectivityMonitorService. The service holds an IConnectivityDeps& reference and
// never touches AppController internals directly.
// Lifetime contract mirrors the sibling deps interfaces: the implementer (GridContextDepsAdapter)
// is owned by AppController and outlives the service. The single std::future the FSM launches is
// drained by ConnectivityMonitorService::DrainProbeFuture, which ~AppController invokes (after
// shuttingDown_.store(true), before JoinBackgroundTasks) while the adapter is still live.
// FROZEN-vs-LIVE FOCUS (the load-bearing distinction): the connectivity FSM re-resolves the
// FOCUSED context's backend and field catalog LIVE on every call (the production code reads
// focusedContext().Backend and fieldCatalog(), both of which re-resolve focusedContextPtr_ per
// call). The existing adapter overrides BackendShared()/KickTimeFieldCatalog() forward to the
// FROZEN per-context ctx_ that the adapter captured at construction. The connectivity accessors
// below are therefore named DISTINCTLY (FocusedBackendShared, FocusedFieldCatalog*) so they get
// their OWN adapter override bodies that forward to app_.BackendShared()/app_.fieldCatalog()
// (LIVE focus). Reusing the frozen-ctx_ overrides would silently break multi-pane behaviour after
// a focus switch. This inverts the reuse-shared-signature pattern the prior phases relied on.
// CATALOG LOCK DISCIPLINE: the catalog-warning reads/clears below stay UNLOCKED, preserving the
// current UI-thread-only single-kick-time-latch discipline. The adapter forwards to the focused
// catalog WITHOUT taking availableFieldsMutex_; do not add that lock here (it would introduce a
// lock-ordering edge against the warm worker that already holds it).
// Test fixtures implement this interface directly (see tests/support/FakeConnectivityDeps.h) so
// unit tests can exercise ConnectivityMonitorService without constructing an AppController.

#include <chrono>
#include <memory>
#include <string>

class ITrackerBackend;

class IConnectivityDeps {
  public:
    virtual ~IConnectivityDeps() = default;

    /// Latched LIVE-focus backend handle (aliasing shared_ptr onto the atomic_load'ed FOCUSED
    /// context backend). The probe worker captures this so a live backend swap / context
    /// retirement can never dangle the backend mid-probe (ADR-0012). Distinct from the frozen-ctx_
    /// BackendShared() the prior phases use — this one re-resolves focus LIVE. Null when no backend.
    virtual std::shared_ptr<ITrackerBackend> FocusedBackendShared() const = 0;

    /// True once shutdown has begun — the FSM bails out of launching new probes. Same signature as
    /// IEditMetaDeps::IsShuttingDown; the one adapter override satisfies both interfaces.
    virtual bool IsShuttingDown() const = 0;

    /// LIVE-focus catalog error/warning reads. Return const-ref (no allocation) so the per-frame
    /// banner formatter stays cheap and callers may bind a reference. Forward to the focused
    /// context's catalog UNLOCKED (UI-thread-only contract); do not take availableFieldsMutex_.
    virtual const std::string& FocusedFieldCatalogError() const = 0;
    /// Whether FocusedFieldCatalogError is transport-shaped — classified at the catalog fetch's
    /// flatten seam (N12 item 13b); the degraded-probe-interval check branches on this.
    virtual bool FocusedFieldCatalogErrorTransient() const = 0;
    virtual const std::string& FocusedFieldCatalogWarning() const = 0;

    /// Clear the LIVE-focus catalog warning and bump its revision. Paired so a recovery / live
    /// success can drop a stale offline catalog banner and force a UI re-evaluation. The adapter
    /// latches the focused catalog once and performs both under the UI-thread-only contract.
    virtual void ClearFocusedFieldCatalogWarning() = 0;
    virtual void BumpFocusedFieldCatalogRevision() = 0;

    /// Forward to OfflineQueueService::PushReplayTimersForward — push replay timers forward while
    /// the transport is down (no point retrying replay against an unreachable server). AppController
    /// routes this so the two services need no direct knowledge of each other. The null-queue guard
    /// lives in the adapter.
    virtual void PushReplayTimers(std::chrono::steady_clock::time_point pushTo) = 0;

    /// Forward to OfflineQueueService::RestartReplayTimersNow — reset replay timers to now when
    /// transport reachability is regained (connectivity recovery). The null-queue guard lives in
    /// the adapter.
    virtual void RestartReplayTimers(std::chrono::steady_clock::time_point now) = 0;
};
