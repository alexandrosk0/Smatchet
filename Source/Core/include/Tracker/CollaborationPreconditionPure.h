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
/// Ordering is load-bearing: read-only is reported BEFORE a missing backend, so a read-only user
/// who triggers a mutation sees the actionable "disable read-only mode" message rather than a
/// transient "backend not initialized" that depends on connection timing. Backend-present is
/// checked before collaboration-capability because `hasCollaboration` is only meaningful once a
/// backend exists (the caller computes it as backend && backend->Collaboration()).
/// `backendAbsentMessage` defaults to the tracker-agnostic text but is overridable because some
/// delegators historically surfaced a differently-worded "backend not initialized" string
/// (e.g. "Jira backend is not initialized."); passing it preserves each call site's exact wording
/// so the flip onto this seam stays behaviour-preserving.
inline VoidResult
ClassifyCollaborationPrecondition(bool readOnlyMode, bool requireWritable, bool hasBackend, bool hasCollaboration,
                                  const std::string& backendAbsentMessage = "Tracker backend is not initialized.") {
    if (requireWritable && readOnlyMode) {
        return VoidResult::Err(std::string("Read-only mode is enabled in Preferences."));
    }
    if (!hasBackend) {
        return VoidResult::Err(backendAbsentMessage);
    }
    if (!hasCollaboration) {
        return VoidResult::Err(std::string("Tracker backend does not support collaboration features."));
    }
    return VoidOk();
}

/// Fallback message for a failing TrackerError whose Detail is empty. Callers encode success as an
/// empty string (e.g. the watch-self path in TrackerGridFieldDisplay.cpp does
/// `r.has_value() ? std::string() : r.error()` and only reports a failure when that string is
/// non-empty), so an `Err("")` would render exactly like success and swallow the failure.
inline const char* CollaborationErrorFallbackMessage() { return "Tracker collaboration request failed."; }

/// Map an ITrackerCollaboration call's TrackerError to a VoidResult: Ok → VoidOk(); any error →
/// its Detail as the message. The historical bool+outError contract surfaced exactly `err.Detail`
/// to the caller, so this preserves the user-visible text one-for-one — except for an empty
/// Detail, which is replaced by the fallback so a failure can never surface as success.
inline VoidResult CollaborationErrorToVoidResult(const TrackerError& err) {
    if (err.IsOk()) {
        return VoidOk();
    }
    if (err.Detail.empty()) {
        return VoidResult::Err(std::string(CollaborationErrorFallbackMessage()));
    }
    return VoidResult::Err(err.Detail);
}

/// Read-side sibling of CollaborationErrorToVoidResult: maps a payload-bearing backend call
/// (`Result<T, TrackerError>`) onto the AppController-facing `Result<T>` — Ok passes the payload
/// through by move; an error surfaces `err.Detail`, exactly as the old `bool + outError` read
/// contract set `outError = err.Detail`. The read delegators (FetchIssueComments / FetchUserGroupNames /
/// FetchPaneGroupMembers / …) keep their divergent backend/capability guards inline (Collaboration()
/// vs Activity(), no read-only gate), so only this error mapper is shared. An empty Detail gets the
/// same non-empty fallback as the write side, for the same reason.
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
