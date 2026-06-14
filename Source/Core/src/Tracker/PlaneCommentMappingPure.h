#ifndef SMATCHET_PLANE_COMMENT_MAPPING_PURE_H
#define SMATCHET_PLANE_COMMENT_MAPPING_PURE_H

// issue-comments PR-C — pure-logic JSON → TrackerIssueComment mapping, extracted
// out of PlaneClient.cpp (which pulls cpr) so the doctest rig can exercise it
// without HTTP. Mirrors the GitHubCommentMappingPure split convention (pure
// mappers live in their own cpr-free / SQLite-free TU).

#include "ITrackerCollaboration.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace plane {

/// Map a Plane `/work-items/{uuid}/comments/` JSON array into the
/// backend-agnostic `TrackerIssueComment` shape. Pure — no I/O, unit-testable.
/// Per element: `Id` = `id` (string, "" if missing or non-string), `Author` =
/// `actor_detail.display_name` (falling back to `created_by` when the nested
/// actor object is absent), `Body` = `comment_stripped` (plain text — never the
/// rich `comment_html`, per the UI plain-text contract; "" if absent),
/// `CreatedAtSec` / `UpdatedAtSec` = epoch seconds parsed from the ISO-8601
/// `created_at` / `updated_at` strings (via ParseIso8601ToUnixSec).
/// `UpdatedAtSec` defaults to `CreatedAtSec` when `updated_at` is absent or
/// unparseable.
/// Pillar 3 — tolerant of a non-array / null / partial payload: a non-array
/// argument returns an empty vector; missing/wrong-typed fields fall back to
/// defaults. Never throws.
std::vector<TrackerIssueComment> MapPlaneIssueComments(const nlohmann::json& nodesArray);

} // namespace plane
} // namespace smatchet

#endif // SMATCHET_PLANE_COMMENT_MAPPING_PURE_H
