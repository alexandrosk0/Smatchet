#ifndef SMATCHET_COMMENT_BLOB_FORMAT_PURE_H
#define SMATCHET_COMMENT_BLOB_FORMAT_PURE_H

// Comments-cell tooltip blob — the ONE formatter behind fieldValues["comment"].
// Every producer (the Jira search mapper via ParseComments, the lazy hover fetch,
// the comments-modal post-back refresh) converges here so all three tracker
// backends render an identical thread and the text is built once, off the draw
// loop — the grid cell only *references* the stored string per frame.
// The blob is Markdown: the tooltip renders it through the same
// MarkdownPreviewRender path as the description tooltip. The History blob
// (ParseChangelog) shares the entry header + separator helpers below.
// Pure — no I/O, no Logger.h (doctest purity, mirrors GitHubIssueSearchMapping).

#include "ITrackerCollaboration.h"

#include <string>
#include <vector>

namespace smatchet {
namespace tracker {

/// Markdown between two activity entries (comments / history): a thematic break
/// surrounded by blank lines so it can never be read as a setext underline.
constexpr const char* kActivityEntrySeparator = "\n---\n\n";

/// Normalise comment text for the grid tooltip: rewrites UTF-8 bullets
/// (U+2022) to "* ". Everything else passes through byte-for-byte.
std::string CleanCommentOutputAscii(const std::string& input);

/// Backslash-escape every Markdown-significant ASCII punctuation character so
/// `text` renders literally as one inline run, and fold CR/LF into spaces so a
/// value can never open a new block. For plain-text values (author names,
/// changelog field values) embedded in a Markdown blob.
std::string EscapeMarkdownInline(const std::string& text);

/// If `md` ends inside an open fenced code block (``` or ~~~), append the
/// matching closing fence so the block cannot swallow whatever the caller
/// concatenates after it (the next comment in the thread). Balanced input is
/// returned unchanged.
std::string CloseOpenCodeFence(const std::string& md);

/// Markdown header line for one activity entry: `**Author** YYYY-MM-DD`.
/// The author is escaped; an empty author becomes "Unknown"; an empty date
/// drops the trailing space.
std::string ActivityEntryHeader(const std::string& author, const std::string& date);

/// Flatten a comment thread into the Markdown blob the Comments-cell tooltip
/// shows. Per entry:
///   **Author** YYYY-MM-DD
///   (blank line)
///   markdown body
/// entries joined by kActivityEntrySeparator, NEWEST FIRST (sorted by
/// CreatedAtSec descending — callers need not pre-order). Dates render as the UTC
/// calendar day; a non-positive CreatedAtSec renders no date. Bodies pass through
/// CleanCommentOutputAscii and CloseOpenCodeFence; entries with an empty body are
/// skipped. Caps: at most 20 comments and 12000 chars total, truncating at an
/// entry boundary. Empty/no-usable-input yields an empty string. Never throws.
std::string FormatCommentBlob(const std::vector<TrackerIssueComment>& comments);

} // namespace tracker
} // namespace smatchet

#endif // SMATCHET_COMMENT_BLOB_FORMAT_PURE_H
