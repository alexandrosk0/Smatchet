#pragma once

// CollaborationPreconditionPure — the error mapping shared by the AppController collaboration READ
// delegators (FetchIssueComments / FetchUserGroupNames / FetchPaneGroupMembers / …): a backend call's
// TrackerError becomes the user-facing message. Tracker WRITES (comments, worklogs, watchers) go
// through the pending-action queue instead (Sync/PendingActionQueueService.h), which owns their
// read-only / backend / capability checks. Pure, so the mapping is unit-tested without AppController.

#include "SmatchetResult.h"
#include "Tracker/TrackerError.h"

#include <string>

namespace smatchet {
namespace collab {

/// Fallback message for a failing TrackerError whose Detail is empty, so a failure never surfaces as
/// an empty, success-looking message.
inline const char* CollaborationErrorFallbackMessage() { return "Tracker collaboration request failed."; }

/// Maps a payload-bearing backend call (`Result<T, TrackerError>`) onto the AppController-facing
/// `Result<T>`: Ok passes the payload through by move; an error surfaces `err.Detail` (the fallback
/// when it is empty). The read delegators keep their backend / capability guards inline
/// (Collaboration() vs Activity(), no read-only gate); only this mapping is shared.
template <typename T> inline Result<T> CollaborationResultToResult(Result<T, TrackerError>&& backendResult) {
    if (backendResult.has_value()) {
        return Result<T>::Ok(std::move(backendResult.value()));
    }
    const std::string& detail = backendResult.error().Detail;
    if (detail.empty()) {
        return Result<T>::Err(std::string(CollaborationErrorFallbackMessage()));
    }
    return Result<T>::Err(detail);
}

} // namespace collab
} // namespace smatchet
