#ifndef SMATCHET_COMMENT_BLOB_FORMAT_PURE_H
#define SMATCHET_COMMENT_BLOB_FORMAT_PURE_H

// Comments-cell tooltip blob — the ONE formatter behind fieldValues["comment"].
// Every producer (the Jira search mapper via ParseComments, the lazy hover fetch,
// the comments-modal post-back refresh) converges here so all three tracker
// backends render an identical thread and the text is built once, off the draw
// loop — the grid cell only *references* the stored string per frame.
// Pure — no I/O, no Logger.h (doctest purity, mirrors GitHubIssueSearchMapping).

#include "ITrackerCollaboration.h"

#include <string>
#include <vector>

namespace smatchet {
namespace tracker {

/// Normalise comment text for the grid tooltip: rewrites UTF-8 bullets
/// (U+2022) to "* ". Everything else passes through byte-for-byte.
std::string CleanCommentOutputAscii(const std::string& input);

/// Flatten a comment thread into the display blob the Comments-cell tooltip
/// shows. Layout (matches the original Jira ParseComments output):
///   [Author] YYYY-MM-DD
///   body text
/// with a blank line between entries, NEWEST FIRST (sorted by CreatedAtSec
/// descending — callers need not pre-order). Dates render as the UTC calendar
/// day; a non-positive CreatedAtSec renders an empty date. Bodies pass through
/// CleanCommentOutputAscii; entries with an empty body are skipped. Caps: at
/// most 20 comments and 12000 chars total, truncating at an entry boundary.
/// Empty/no-usable-input yields an empty string. Never throws.
std::string FormatCommentBlob(const std::vector<TrackerIssueComment>& comments);

} // namespace tracker
} // namespace smatchet

#endif // SMATCHET_COMMENT_BLOB_FORMAT_PURE_H
