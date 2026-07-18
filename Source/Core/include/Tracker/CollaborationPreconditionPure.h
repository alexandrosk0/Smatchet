#pragma once

// CollaborationPreconditionPure — the pure guard-ordering + error-mapping shared by the
// AppController collaboration delegators (AddIssueWatcher / FetchIssueWatchers / FetchIssueVotes /
// AddIssueCommentPlain / AddWorklog / …). Each delegator latches the live backend (an
// AppController-thread concern that cannot be pure), then runs the SAME preflight — read-only mode,
// backend present, collaboration capability present — before calling through ITrackerCollaboration
// and mapping the returned TrackerError to a user-facing outcome.
//
// Extracted + unit-tested here because AppController.cpp is not part of the doctest link rig (it
// drags the UI/Lua/MCP chain), so the guard ORDERING and the exact user-facing strings a delegator
// must preserve cannot otherwise be pinned by a test. This is the seam the pending #21
// bool+outError → VoidResult flip of those delegators lands on: the branch logic is already
// VoidResult-shaped and verified, so the public flip is a thin signature change.

#include "SmatchetResult.h"
#include "Tracker/TrackerError.h"

#include <string>

namespace smatchet {
namespace collab {

/// Preflight for a collaboration call, returned as a VoidResult so a delegator can surface the
/// message verbatim. `requireWritable` is true for MUTATIONS (AddIssueWatcher, AddWorklog, comment
/// posts) — those are blocked in read-only mode; a read-only fetch passes false and skips that gate.
///
/// Ordering is load-bearing: read-only is reported BEFORE a missing backend, so a read-only user
/// who triggers a mutation sees the actionable "disable read-only mode" message rather than a
/// transient "backend not initialized" that depends on connection timing. Backend-present is
/// checked before collaboration-capability because `hasCollaboration` is only meaningful once a
/// backend exists (the caller computes it as backend && backend->Collaboration()).
inline VoidResult ClassifyCollaborationPrecondition(bool readOnlyMode, bool requireWritable, bool hasBackend,
                                                    bool hasCollaboration) {
    if (requireWritable && readOnlyMode) {
        return VoidResult::Err(std::string("Read-only mode is enabled in Preferences."));
    }
    if (!hasBackend) {
        return VoidResult::Err(std::string("Tracker backend is not initialized."));
    }
    if (!hasCollaboration) {
        return VoidResult::Err(std::string("Tracker backend does not support collaboration features."));
    }
    return VoidOk();
}

/// Map an ITrackerCollaboration call's TrackerError to a VoidResult: Ok → VoidOk(); any error →
/// its Detail as the message. The historical bool+outError contract surfaced exactly `err.Detail`
/// to the caller, so this preserves the user-visible text one-for-one.
inline VoidResult CollaborationErrorToVoidResult(const TrackerError& err) {
    if (err.IsOk()) {
        return VoidOk();
    }
    return VoidResult::Err(err.Detail);
}

} // namespace collab
} // namespace smatchet
