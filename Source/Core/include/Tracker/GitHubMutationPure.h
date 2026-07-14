#ifndef SMATCHET_GITHUB_MUTATION_PURE_H
#define SMATCHET_GITHUB_MUTATION_PURE_H

// Pure (cpr-free) request-builders + response-parsers for the GitHub issue write
// surface (the UpdateIssueFields slice of docs/plans/shipped/github-tracker-backend.md
// § Remaining), split out of the
// cpr-bound GitHubClient.cpp exactly like LinearMutationPure / PlaneCustomPropertyPure
// split their wire logic. Keeping the fields→PATCH-body translation, the milestone
// lookup, and the label-set extraction in their own TU lets the doctest rig pin the
// exact wire contract without a network stack. Null/missing-safe per Pillar 3.

#include "SmatchetResult.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace github {

/// PATCH plan for one `UpdateIssueFields` call, translated from the catalog-id-keyed
/// payload `GitHubClient::BuildFieldPayload` emits ({"summary": ...}, {"labels": [...]}).
/// - `PatchBody` carries the GitHub REST names (title / body / state / assignees, plus
///   `milestone` once the caller resolves a title to its number).
/// - Labels are NOT in `PatchBody`: the set-replace contract reconciles against GitHub's
///   additive label API via pre-fetch + `ComputeLabelEditDiff` (plan decision 4).
/// - Milestone arrives as the display title (`GitHubIssueSearchMapping` stores the
///   title); PATCH wants the milestone NUMBER. An all-digit value is used as the number
///   directly; anything else sets `NeedsMilestoneResolve` and the caller looks the
///   title up via GET /repos/{o}/{r}/milestones.
struct GitHubIssueUpdatePlan {
    nlohmann::json PatchBody = nlohmann::json::object();
    bool HasLabelsEdit = false;
    std::vector<std::string> LabelsIntended;
    bool NeedsMilestoneResolve = false;
    std::string MilestoneTitle;
};

/// Translate a catalog-id-keyed fields payload into the PATCH plan. Editable ids:
/// `summary` (→ title; non-empty required), `description` (→ body; empty clears),
/// `status` (→ state; "open"/"closed", case-insensitive), `assignee` (→ assignees;
/// JSON string array or CSV string, empty clears), `labels` (array or CSV; empty
/// clears all), `milestone` (empty clears). Any other id → Err (read-only on GitHub).
/// A leading "[PR] " on summary is stripped — it is the grid's display prefix
/// (`GitHubIssueSearchMapping`), not part of the real title; without the strip a PR
/// title edit would compound the prefix on every round-trip.
Result<GitHubIssueUpdatePlan, std::string> BuildGitHubIssueUpdatePlan(const nlohmann::json& fields);

/// Exact-title lookup over one GET /repos/{o}/{r}/milestones page (JSON array of
/// {"number": N, "title": "..."}). Returns the milestone number, or 0 when absent /
/// malformed (milestone numbers are 1-based, so 0 is never a real hit).
std::int64_t FindGitHubMilestoneNumberByTitle(const nlohmann::json& milestonesArray, const std::string& title);

/// Label names from a GET /repos/{o}/{r}/issues/{n} body. Tolerates both label shapes
/// GitHub emits ({"name": "bug"} objects and bare strings); blank names are skipped.
std::vector<std::string> ExtractGitHubIssueLabelNames(const nlohmann::json& issueJson);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_MUTATION_PURE_H
