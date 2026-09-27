#pragma once

// PendingActionTypes — tracker actions saved while offline other than issue creates and field
// edits (Quality Pillar 6): comments today, worklogs and watch next. One generic queue keyed by
// kind (DRY) in the pending_actions / pending_actions_dead tables. Plain data and wire names only
// (no SQLite), rank 0 so the cache seam (ISyncCache), the Sync service and the UI facet can all
// include it — the same placement as the create / field-edit queue rows in CachedTicketTypes.h.

#include <cstdint>
#include <string>
#include <vector>

enum class PendingActionKind : unsigned char { CommentAdd, WorklogAdd, WatchAdd };

/// The `kind` column value for a kind.
inline const char* PendingActionKindWire(PendingActionKind kind) {
    switch (kind) {
    case PendingActionKind::CommentAdd:
        return "comment_add";
    case PendingActionKind::WorklogAdd:
        return "worklog_add";
    case PendingActionKind::WatchAdd:
        return "watch_add";
    }
    return "";
}

/// Inverse of PendingActionKindWire. False for a value this build does not know (a row written by
/// a newer build); the caller keeps or archives such a row, never guesses.
inline bool ParsePendingActionKind(const std::string& wire, PendingActionKind& out) {
    const PendingActionKind known[] = {PendingActionKind::CommentAdd, PendingActionKind::WorklogAdd,
                                       PendingActionKind::WatchAdd};
    for (const PendingActionKind kind : known) {
        if (wire == PendingActionKindWire(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

/// The `state` column values.
namespace PendingActionState {
/// Not sent yet (queued offline, or the send failed before the tracker could act on it).
constexpr const char* kPending = "pending";
/// A send is in flight. A row still in this state when the cache opens was interrupted mid-send
/// and becomes kAmbiguous.
constexpr const char* kSending = "sending";
/// The request may have reached the tracker (its response was lost). Replay checks the tracker
/// before sending again where the kind allows it, so the action is not applied twice.
constexpr const char* kAmbiguous = "ambiguous";
/// Ambiguous and not checkable: never resent automatically; the user confirms or discards it.
constexpr const char* kNeedsReview = "needs_review";
} // namespace PendingActionState

/// One row of pending_actions.
struct PendingActionRecord {
    std::int64_t Id = 0;
    std::string BackendKey; ///< cache namespace of the context that queued it (replay filters on it)
    std::string Kind;       ///< PendingActionKindWire value
    std::string IssueKey;
    std::string PayloadJson; ///< kind-specific JSON (e.g. {"body", "created"} for a comment)
    std::string State;       ///< PendingActionState value
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
};

/// One row of pending_actions_dead: an action that will not be retried automatically. `Row` is the
/// queued row as it was when archived — `Row.Id` is its original id, `Row.State` the state a restore
/// resumes from, `Row.LastError` the terminal error.
struct DeadPendingAction {
    std::int64_t DeadId = 0;
    std::int64_t ArchivedAtEpochSec = 0;
    std::string TerminalReason;
    PendingActionRecord Row;
};

/// The queue as last read from the cache by a worker. The UI reads only this (Pillar 2: no SQLite
/// per frame); every write through PendingActionQueueService republishes it.
struct PendingActionsSnapshot {
    std::vector<PendingActionRecord> Pending;
    std::vector<DeadPendingAction> Dead;
};

/// Outcome of submitting an action (PendingActionQueueService::SubmitOrQueue).
struct PendingActionSubmitResult {
    enum class Kind : unsigned char { Sent, Queued, Failed };
    Kind K = Kind::Failed;
    std::int64_t QueueId = 0;
    /// Queued because a live send just failed on the network (not because the last probe already said
    /// offline): the caller should probe connectivity now.
    bool QueuedAfterNetworkFailure = false;
    std::string Error;
};
