#ifndef SMATCHET_COMMENT_BLOB_FORMAT_PURE_H
#define SMATCHET_COMMENT_BLOB_FORMAT_PURE_H

// Comments-cell tooltip blob — the ONE formatter behind fieldValues["comment"].
// Every producer (the Jira search mapper via ParseComments, the lazy hover fetch,
// the comments-modal post-back refresh) converges here so all three tracker
// backends render an identical thread and the text is built once, off the draw
// loop — the grid cell only *references* the stored string per frame.
// The blob is Markdown: the tooltip renders it through the same
// MarkdownPreviewRender path as the description tooltip. The History tooltip
// reuses the entry header + separator via PlainActivityBlobToMarkdown.
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

/// Backslash-escape every ASCII punctuation character (CommonMark allows escaping any of
/// them) so `text` renders literally as one inline run, and fold CR/LF into spaces so it
/// can never open a new block. For plain text shown through the Markdown renderer.
std::string EscapeMarkdownText(const std::string& text);

/// If `md` ends inside an open fenced code block (``` or ~~~), append the
/// matching closing fence, at the opener's indent, so the block cannot swallow
/// whatever the caller concatenates after it (the next comment in the thread).
/// A backtick run followed by another backtick on its line is inline code, not a
/// fence. Balanced input is returned unchanged.
std::string CloseOpenCodeFence(const std::string& md);

/// Markdown header line for one activity entry: `**Author** YYYY-MM-DD`. Only
/// `\`, `*`, backtick and `<` are escaped in the author, so a stored blob stays
/// searchable by name; an empty author becomes "Unknown"; an empty date drops the
/// trailing space.
std::string ActivityEntryHeader(const std::string& author, const std::string& date);

/// Render-time view of a plain activity blob (the History field, which ParseChangelog
/// stores as plain "[Author] date" / "field: from -> to" text so the grid cell and the
/// text filter keep reading it verbatim) as Markdown in the Comments-tooltip shape:
/// each "[Author] date" line that starts an entry becomes an ActivityEntryHeader, entries
/// are joined by kActivityEntrySeparator, and every other line is escaped
/// (EscapeMarkdownText) with its line breaks kept. Content is never dropped.
std::string PlainActivityBlobToMarkdown(const std::string& blob);

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
