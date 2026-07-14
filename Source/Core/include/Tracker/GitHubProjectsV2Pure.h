#ifndef SMATCHET_GITHUB_PROJECTS_V2_PURE_H
#define SMATCHET_GITHUB_PROJECTS_V2_PURE_H

// Pure (cpr-free) GraphQL documents + response parsers + value builders for the
// GitHub Projects v2 custom-field surface (docs/plans/shipped/github-projects-v2-fields.md).
// Same pure-seam shape as LinearMutationPure / PlaneCustomPropertyPure: the wire
// contract (documents, JSON→struct parsing, grid-string→typed-value building) lives
// here so the doctest rig pins it hermetically; the cpr-bound orchestration lives in
// GitHubProjectsV2.cpp. Null/missing-safe per Pillar 3.
//
// Field-id scheme: catalog ids are lowercase slugs "pv2.<name-slug>" — the editmeta
// cache lower-cases lookup keys, so a mixed-case GraphQL node id could never match.
// The case-SENSITIVE ProjectV2 field node id travels in TrackerField::SchemaCustom
// and inside the BuildFieldPayload output, never as the catalog id.

#include "SmatchetResult.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace github {

// === GraphQL documents (exposed so tests pin the exact wire strings) ===

/// Project lookup by owner login + project number. Queries BOTH organization()
/// and user() — exactly one resolves for a given login, the other contributes a
/// (tolerated) errors[] entry with data:null on its half.
const char* ProjectV2FieldsQueryDocument();
/// Paginated items listing for one project node id, with per-item content
/// (issue/PR number + repo) and typed field values.
const char* ProjectV2ItemsQueryDocument();
/// Resolve the project item id for one issue/PR (owner, repo, number) —
/// `issueOrPullRequest.projectItems` matched against the project node id by the
/// caller (ParseProjectV2ItemIdForProject).
const char* ProjectV2ItemForIssueQueryDocument();
/// updateProjectV2ItemFieldValue mutation (typed `value` object).
const char* ProjectV2UpdateFieldValueMutationDocument();
/// clearProjectV2ItemFieldValue mutation (empty grid value = clear).
const char* ProjectV2ClearFieldValueMutationDocument();

// === Parsed shapes ===

/// The project's identity + the custom-field slice of its field list.
struct ProjectV2Catalog {
    std::string ProjectNodeId;
    std::string ProjectTitle;
    std::vector<TrackerField> Fields; // Id = "pv2.<slug>", SchemaCustom = field node id
};

/// One item's join key + its field values keyed by the catalog's "pv2.<slug>" ids.
struct ProjectV2ItemValues {
    std::string TicketKey; // "owner/repo#N" — joins onto CachedTicket::id
    std::vector<std::pair<std::string, std::string>> Values;
};

/// One parsed items page.
struct ProjectV2ItemsPage {
    std::vector<ProjectV2ItemValues> Items;
    bool HasNext = false;
    std::string EndCursor;
};

// === Parsers (JSON body → struct; Err carries a human-readable message) ===

/// Parse the ProjectV2FieldsQueryDocument response. Tolerates the org/user split
/// (errors[] entries are fine as long as one half resolved a project). Keeps only
/// the field data types the grid can represent as custom columns — TEXT, NUMBER,
/// DATE, SINGLE_SELECT (with options), ITERATION (read-only) — and skips the
/// built-in projections (TITLE / ASSIGNEES / LABELS / MILESTONE / REPOSITORY /
/// LINKED_PULL_REQUESTS / REVIEWERS / TRACKS / TRACKED_BY / …) that duplicate the
/// native catalog columns. Slugs collide → "-2"/"-3" suffixes keep ids unique.
Result<ProjectV2Catalog, std::string> ParseProjectV2Catalog(const nlohmann::json& body);

/// Parse one ProjectV2ItemsQueryDocument response page. Items whose content is
/// not an issue/PR (draft items) or lacks a resolvable repo are skipped. Values
/// map to display strings: text verbatim, number via minimal formatting, date
/// "YYYY-MM-DD", single-select option NAME, iteration title. `catalogFields`
/// provides the field-node-id → "pv2.<slug>" mapping (from ParseProjectV2Catalog).
Result<ProjectV2ItemsPage, std::string> ParseProjectV2ItemsPage(const nlohmann::json& body,
                                                                const std::vector<TrackerField>& catalogFields);

/// Parse the ProjectV2ItemForIssueQueryDocument response: the item id of this
/// issue/PR inside `projectNodeId`, or Ok("") when the issue is not an item of
/// that project. Err on malformed responses.
Result<std::string, std::string> ParseProjectV2ItemIdForProject(const nlohmann::json& body,
                                                                const std::string& projectNodeId);

// === Value building (grid strings → the mutation's ProjectV2FieldValue) ===

/// Build the typed `value` object for updateProjectV2ItemFieldValue from the
/// set-replace values of one edit. Empty/blank values yield Ok(null) — the caller
/// routes that to the clear mutation. Families: Text → {text}, Number → {number}
/// (validated), Date → {date "YYYY-MM-DD"} (validated), SelectSingle →
/// {singleSelectOptionId} resolved from the field's options by id or display name
/// (Err when the value matches no option). Iteration / unknown families → Err
/// (read-only in this slice).
Result<nlohmann::json, std::string> BuildProjectV2FieldValue(const TrackerField& field,
                                                             const std::vector<std::string>& values);

/// Lowercase slug for a field display name: ASCII-lowered, runs of
/// non-alphanumerics collapse to single '-', trimmed of leading/trailing '-'.
/// Empty input → "field".
std::string SlugifyProjectV2FieldName(const std::string& name);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_PROJECTS_V2_PURE_H
