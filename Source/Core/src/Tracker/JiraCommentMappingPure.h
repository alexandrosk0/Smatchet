#ifndef SMATCHET_JIRA_COMMENT_MAPPING_PURE_H
#define SMATCHET_JIRA_COMMENT_MAPPING_PURE_H

// issue-comments PR-B — pure-logic Jira comment-node array → TrackerIssueComment
// mapping, extracted out of JiraIssueSearch.cpp (which pulls cpr) so the doctest
// rig can exercise it without HTTP. Mirrors the GitHubCommentMappingPure split
// convention (pure mappers live in their own cpr-free / SQLite-free TU).

#include "ITrackerCollaboration.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace jira {

/// Map a Jira `/rest/api/3/issue/{key}/comment` comment-node array (the flattened
/// `comments` arrays across pages, as gathered by JiraFetchIssueCommentsPages)
/// into the backend-agnostic `TrackerIssueComment` shape. Pure — no I/O, unit-testable.
/// Per element: `Id` = `id` (string), `Author` = `author.displayName` ("Unknown"
/// if absent, via ParseCommentAuthor), `Body` = the ADF `body` flattened to plain
/// text (via AdfBodyToPlainText — the Body contract is plain text only),
/// `CreatedAtSec` / `UpdatedAtSec` = epoch seconds parsed from the ISO-8601
/// `created` / `updated` strings (via smatchet::github::ParseIso8601ToUnixSec, which
/// tolerates Jira's millisecond precision). `UpdatedAtSec` defaults to `CreatedAtSec`
/// when `updated` is absent or unparseable.
/// Pillar 3 — tolerant of a non-array / null / partial payload: a non-array argument
/// returns an empty vector; missing/wrong-typed fields fall back to defaults. Never throws.
std::vector<TrackerIssueComment> MapJiraIssueComments(const nlohmann::json& nodesArray);

} // namespace jira
} // namespace smatchet

#endif // SMATCHET_JIRA_COMMENT_MAPPING_PURE_H
