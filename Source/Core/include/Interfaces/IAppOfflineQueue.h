#ifndef SMATCHET_INTERFACES_IAPP_OFFLINE_QUEUE_H
#define SMATCHET_INTERFACES_IAPP_OFFLINE_QUEUE_H

// Narrow facet for the offline pending / dead-letter queue admin surface —
// AppController fan-in Phase 5 (docs/plans/appcontroller-fan-in-phase5-facets.md).
// AppController implements this interface; command TUs (BuiltinCommands_Offline.cpp)
// depend on it by reference instead of on the full AppController.h, dropping off the
// AppController.h fan-in count. None of these methods is per-frame-hot, so virtualising
// them carries no Pillar-1 inlining cost (the plan's per-frame-getter exclusion rule).
//
// Placement: rank-0 leaf (Source/Core/include/Interfaces/, a non-layer-prefix subdir →
// include_cycle_audit._layer_rank returns 0), so any layer may include it. The collection
// element types live in the rank-0 CachedTicketTypes.h (included below); the two delete-
// summary return types live in Sync/OfflineQueueTypes.h (layer rank 2) — a rank-0 header
// cannot include a Sync/ header without a low->high back-edge, so they are forward-declared.
// A by-value return of an incomplete type is well-formed in a pure-virtual declaration; a
// caller that reads a summary's fields includes Sync/OfflineQueueTypes.h itself, per IWYU.

#include "CachedTicketTypes.h" // PendingCreate / PendingFieldEditRecord / DeadPendingCreate / DeadPendingFieldEdit

#include <cstddef>
#include <cstdint>
#include <vector>

struct DeadLetterDeleteSummary;
struct DeadFieldEditDeleteSummary;

class IAppOfflineQueue {
  public:
    virtual ~IAppOfflineQueue() = default;

    /// Kick a replay attempt over the queued (pending) offline creates / field edits.
    virtual void TickOfflineCreates() = 0;
    virtual void TickOfflineFieldEdits() = 0;

    /// Queued (not-yet-dead) offline work — inspection.
    virtual std::size_t GetPendingCreateCount() const = 0;
    virtual std::vector<PendingCreate> GetPendingCreates() const = 0;
    virtual std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const = 0;

    /// Dead-letter (permanently-failed) offline work — inspection + prune.
    virtual std::vector<DeadPendingCreate> GetDeadPendingCreates() const = 0;
    virtual std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const = 0;
    virtual DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) = 0;
    virtual DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_OFFLINE_QUEUE_H
