#ifndef SMATCHET_GITHUB_COMMENT_MAPPING_PURE_H
#define SMATCHET_GITHUB_COMMENT_MAPPING_PURE_H

// issue-comments PR-A — pure-logic JSON → TrackerIssueComment mapping, extracted
// out of GitHubClient.cpp (which pulls cpr) so the doctest rig can exercise it
// without HTTP. Mirrors the GitHubIssueSearchMapping split convention (pure
// mappers live in their own cpr-free / SQLite-free TU).

#include "ITrackerCollaboration.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace github {

/// Map a GitHub `/repos/{o}/{r}/issues/{n}/comments` JSON array into the
/// backend-agnostic `TrackerIssueComment` shape. Pure — no I/O, unit-testable.
/// Per element: `Id` = `id` (int64 → std::to_string), `Author` = `user.login`
/// (string, "" if missing), `Body` = `body` (string, "" if missing),
/// `CreatedAtSec` / `UpdatedAtSec` = epoch seconds parsed from the ISO-8601
/// `created_at` / `updated_at` strings (via ParseIso8601ToUnixSec).
/// `UpdatedAtSec` defaults to `CreatedAtSec` when `updated_at` is absent or
/// unparseable.
/// Pillar 3 — tolerant of a non-array / null / partial payload: a non-array
/// argument returns an empty vector; missing/wrong-typed fields fall back to
/// defaults. Never throws.
std::vector<TrackerIssueComment> MapGitHubIssueComments(const nlohmann::json& commentsArray);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_COMMENT_MAPPING_PURE_H
