#pragma once

// IFieldEditDeps — narrow interface bundle exposing every AppController-side dependency that
// FieldEditPipelineService reaches into (CommitOrQueue / ApplyFieldEditResult + their helpers).
// Mirrors the IEditMetaDeps / ITicketSyncDeps / IOfflineQueueDeps template: AppController constructs
// `GridContextDepsAdapter` (which implements this interface alongside the others) and hands it to
// `FieldEditPipelineService`. The service holds an `IFieldEditDeps&` reference and never touches
// AppController internals directly.
// Pane-scoped (#2260): an edit is bound to the pane the user acted in (a latched PendingActionTarget),
// so the tickets it reads, the local update it applies and the queue row it writes all name that pane.
// Only LatchFocusedPaneTarget reads focus, to bind an edit that arrived unbound. The offline-queue DB
// write is reached through EnqueueOfflineFieldEdit, which CommitOrQueue calls on the field-edit worker
// (Quality Pillar 6 queue-first), so the service never names OfflineQueueService or SQLite. Edit-
// metadata operations are reached through the EditMetaCacheService reference the service holds
// directly (ctor-injected), not through this interface.
// Lifetime contract mirrors IEditMetaDeps: the implementer (GridContextDepsAdapter) is owned by
// AppController and outlives the FieldEditPipelineService.
// Test fixtures implement this interface directly (see tests/support/FakeFieldEditDeps.h) so unit
// tests can exercise FieldEditPipelineService without constructing an AppController.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CachedTicketTypes.h"  // CachedTicket (per-pane snapshot + UpdateTicketFor arg)
#include "PendingActionTypes.h" // PendingActionTarget (the pane an edit is bound to; its last probe)

class IFieldEditDeps {
  public:
    virtual ~IFieldEditDeps() = default;

    /// True when the local cache is initialized. A PREDICATE, not the raw `Cache*` — keeps the
    /// service SQLite-free (ADR-0020 sync-cache purity): the pipeline only needs to know whether a
    /// cache exists before applying an optimistic local update.
    virtual bool HasCache() const = 0;

    /// The focused pane as a latched target: its backend, cache key, pane id and backend generation, plus
    /// the last connectivity probe. Binds an edit that arrived without a target. Any thread.
    virtual PendingActionTarget LatchFocusedPaneTarget() const = 0;

    /// The published tickets of `target`'s pane, or null when that pane was retired or switched tracker
    /// since the target was latched (its backend generation moved). Any thread.
    virtual std::shared_ptr<const std::vector<CachedTicket>>
    TicketsSnapshotFor(const PendingActionTarget& target) const = 0;

    /// Save `ticket` in `target`'s pane — its cache namespace, then that pane's grid — after a saved or queued
    /// edit, so the grid shows it without a re-sync. A no-op when the pane is gone or switched tracker. UI thread.
    virtual void UpdateTicketFor(const PendingActionTarget& target, const CachedTicket& ticket) = 0;

    /// Arm the deferred live-tracker-backend success notify (a successful field edit counts as a
    /// reachable-backend signal). CONST — the SAME overload the existing const
    /// IEditMetaDeps::RequestDeferredLiveTrackerBackendSuccessNotify() declares; the one adapter
    /// override satisfies both interfaces.
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() const = 0;

    /// Worker-safe: persists to the offline queue (SQLite) off the UI thread, under `backendKey` (the
    /// namespace replay filters on). Returns the queue row id, or 0 with `outError` set when the edit
    /// could not be queued (read-only, no cache, DB error).
    virtual std::int64_t EnqueueOfflineFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                                 const std::string& fieldId, const std::string& fieldsPayloadJson,
                                                 const std::string& originalRichValue, const std::string& originalValue,
                                                 bool hasOriginalValue, std::string& outError) = 0;
};
