#pragma once

// PendingActionPolicyPure — the decisions behind the pending-action queue (Quality Pillar 6) that
// keep an offline action exactly-once: which state a failed send leaves it in, whether an
// interrupted comment already reached the tracker, and each kind's payload shape. Pure: no I/O,
// no Logger.h, no SQLite (the doctest rig links it bare).

#include "ITrackerCollaboration.h" // TrackerIssueComment
#include "PendingActionTypes.h"
#include "Tracker/TrackerError.h"

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace pendingaction {

/// How long before its queued time a matching tracker comment still counts as the queued one
/// (clock skew, and a send that landed just before the queue recorded it).
constexpr std::int64_t kCommentDedupeWindowSec = 300;

/// The state a failed send leaves the action in. "" when the failure is final (the tracker
/// rejected it). kPending when the tracker certainly did not act on it
/// (TrackerError::ProvablyNotApplied: never sent, or rate limited), and for a watch in any retryable
/// case (watching twice is harmless). kAmbiguous when it may have acted: a timeout or 5xx after the
/// request went out.
const char* StateAfterFailedSend(PendingActionKind kind, const TrackerError& error);

/// The state an ambiguous comment is left in when the check for it on the tracker fails. A failed
/// check is never the tracker refusing the comment, so it stays kAmbiguous (checked again next pass,
/// until the attempt cap) for any failure — transport, 5xx, 429, an unreadable page. "" (final) only
/// when the check proves the comment can never be resolved: no access (Auth), the issue is gone
/// (NotFound), or the request itself is invalid.
const char* StateAfterFailedDedupeCheck(const TrackerError& error);

/// Comment text reduced to what survives a Markdown → tracker format → Markdown round trip: ASCII
/// letters and digits, lower-cased, plus every non-ASCII byte. Formatting, punctuation and
/// whitespace drop out.
std::string NormalizeCommentForDedupe(const std::string& text);

/// True when `fetched` holds a comment created no earlier than kCommentDedupeWindowSec before
/// `queuedAtSec` whose text matches `body` (NormalizeCommentForDedupe; a body with no letters or
/// digits compares its trimmed, CRLF-normalized text) — an ambiguous send that did land.
bool CommentAlreadyPosted(const std::vector<TrackerIssueComment>& fetched, const std::string& body,
                          std::int64_t queuedAtSec);

/// The CommentAdd payload: {"body": <Markdown text>, "created": <epoch seconds when queued>}.
std::string BuildCommentActionPayload(const std::string& body, std::int64_t createdAtSec);

/// Inverse of BuildCommentActionPayload (bounded parse). False when malformed or the body is empty.
bool ParseCommentActionPayload(const std::string& json, std::string& outBody, std::int64_t& outCreatedAtSec);

/// The WorklogAdd payload: the ITrackerCollaboration::AddWorklog arguments.
struct WorklogActionPayload {
    std::string TimeSpent; ///< e.g. "2h 30m"; required
    std::string TimeRemaining;
    std::string AdjustEstimate; ///< "auto", "new", …; "" leaves the estimate alone
    std::string Description;
    std::string Started; ///< Jira date-time, e.g. "2026-09-27T10:00:00.000+0200"
};

/// {"timeSpent", "timeRemaining", "adjustEstimate", "description", "started"}.
std::string BuildWorklogActionPayload(const WorklogActionPayload& worklog);

/// Inverse of BuildWorklogActionPayload (bounded parse). False when malformed or timeSpent is empty.
bool ParseWorklogActionPayload(const std::string& json, WorklogActionPayload& out);

/// The WatchAdd payload: watching takes no arguments.
constexpr const char* kWatchActionPayload = "{}";

} // namespace pendingaction
} // namespace smatchet
