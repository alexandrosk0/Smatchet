#pragma once

// Pending-edit overlay for the grid (docs/plans/active/delta-sync.md, Review follow-up decisions, local edits).
// A delta reload serves a row whose local edit the tracker has not confirmed yet as "unchanged": the row keeps
// its sync meta, and the grid keeps showing the local value. So the grid says it: every row with a queued edit,
// a queued action (comment / worklog / watch) or a dead-lettered one carries a "pending" cue, and the cell of a
// queued field edit carries one too. The overlay is built from the offline-queue snapshots the workers already
// published (OfflineQueueService::Snapshot / PendingActionQueueService::Snapshot), never from SQLite, and only
// when one of them changes. Header-only, no I/O.

#include "CachedTicketTypes.h"
#include "PendingActionTypes.h"

#include <string>
#include <unordered_map>

namespace smatchet {
namespace sync {

/// How far an unconfirmed edit has got. Ordered: a later value is the worse one, so a row or cell with several
/// edits shows the worst.
enum class PendingEditStatus : unsigned char {
    None = 0,
    Queued = 1,         ///< waiting to be sent (offline, or retrying)
    NeedsAttention = 2, ///< dead-lettered, ambiguous, held for review or in a merge conflict: it will not land alone
};

/// Row key: a tracker namespace and the issue key (the same key on two trackers is two rows).
inline std::string PendingRowKey(const std::string& cacheBackendKey, const std::string& issueKey) {
    return cacheBackendKey + '\x1f' + issueKey;
}
/// Cell key: a row key and the field the edit is for.
inline std::string PendingCellKey(const std::string& cacheBackendKey, const std::string& issueKey,
                                  const std::string& fieldId) {
    return PendingRowKey(cacheBackendKey, issueKey) + '\x1f' + fieldId;
}

/// The unconfirmed edits of the queues, indexed for the per-row and per-cell lookups of the grid.
class PendingOverlay {
  public:
    /// No unconfirmed edit anywhere: the grid skips every lookup.
    bool Empty() const { return rows_.empty(); }

    /// The worst status among the row's edits and actions (None when it has none).
    PendingEditStatus Row(const std::string& cacheBackendKey, const std::string& issueKey) const {
        return Find(rows_, PendingRowKey(cacheBackendKey, issueKey));
    }
    /// The worst status among the edits queued for this field of this row.
    PendingEditStatus Cell(const std::string& cacheBackendKey, const std::string& issueKey,
                           const std::string& fieldId) const {
        return Find(cells_, PendingCellKey(cacheBackendKey, issueKey, fieldId));
    }
    /// How many unconfirmed edits and actions the row has (for the tooltip).
    int RowCount(const std::string& cacheBackendKey, const std::string& issueKey) const {
        const auto it = counts_.find(PendingRowKey(cacheBackendKey, issueKey));
        return it == counts_.end() ? 0 : it->second;
    }

    /// Records one edit. An empty field id marks the row only (an action has no cell).
    void Add(const std::string& cacheBackendKey, const std::string& issueKey, const std::string& fieldId,
             PendingEditStatus status) {
        if (issueKey.empty() || status == PendingEditStatus::None) {
            return;
        }
        const std::string rowKey = PendingRowKey(cacheBackendKey, issueKey);
        Raise(rows_, rowKey, status);
        ++counts_[rowKey];
        if (!fieldId.empty()) {
            Raise(cells_, PendingCellKey(cacheBackendKey, issueKey, fieldId), status);
        }
    }

  private:
    using StatusMap = std::unordered_map<std::string, PendingEditStatus>;
    static PendingEditStatus Find(const StatusMap& map, const std::string& key) {
        const auto it = map.find(key);
        return it == map.end() ? PendingEditStatus::None : it->second;
    }
    static void Raise(StatusMap& map, const std::string& key, PendingEditStatus status) {
        const auto it = map.find(key);
        if (it == map.end()) {
            map[key] = status;
        } else if (status > it->second) {
            it->second = status;
        }
    }

    StatusMap rows_;
    StatusMap cells_;
    std::unordered_map<std::string, int> counts_;
};

/// A queued action's state as a status: still waiting, or stuck on the user (ambiguous / needs review).
inline PendingEditStatus PendingActionStatus(const std::string& state) {
    if (state == PendingActionState::kAmbiguous || state == PendingActionState::kNeedsReview) {
        return PendingEditStatus::NeedsAttention;
    }
    return PendingEditStatus::Queued; // pending, sending, or a state a newer build wrote: still unconfirmed
}

/// The overlay for both queue snapshots (either may be null). Queued field edits mark their cell and row; queued
/// actions mark the row; every dead-lettered edit or action, and a field edit held in a merge conflict, needs
/// attention. Pending creates are not rows of the grid, so they are not part of it.
inline PendingOverlay BuildPendingOverlay(const OfflineQueueSnapshot* queue, const PendingActionsSnapshot* actions) {
    PendingOverlay overlay;
    if (queue != nullptr) {
        for (const PendingFieldEditRecord& edit : queue->PendingEdits) {
            overlay.Add(edit.BackendKey, edit.IssueKey, edit.FieldId,
                        edit.HasMergeConflict ? PendingEditStatus::NeedsAttention : PendingEditStatus::Queued);
        }
        for (const DeadPendingFieldEdit& edit : queue->DeadEdits) {
            overlay.Add(edit.BackendKey, edit.IssueKey, edit.FieldId, PendingEditStatus::NeedsAttention);
        }
    }
    if (actions != nullptr) {
        for (const PendingActionRecord& action : actions->Pending) {
            overlay.Add(action.BackendKey, action.IssueKey, std::string(), PendingActionStatus(action.State));
        }
        for (const DeadPendingAction& dead : actions->Dead) {
            overlay.Add(dead.Row.BackendKey, dead.Row.IssueKey, std::string(), PendingEditStatus::NeedsAttention);
        }
    }
    return overlay;
}

} // namespace sync
} // namespace smatchet
