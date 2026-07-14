#ifndef SMATCHET_GITHUB_PROJECTS_V2_H
#define SMATCHET_GITHUB_PROJECTS_V2_H

// Private header — cpr-bound orchestration for the GitHub Projects v2 field
// surface (docs/plans/shipped/github-projects-v2-fields.md). Lives next to its
// implementation like GitHubIssueSearch.h: only GitHubClient.cpp and
// GitHubProjectsV2.cpp pick it up. The pure wire contract (documents, parsers,
// value builders) is in the public GitHubProjectsV2Pure.h; this layer only adds
// the GraphQL POSTs, pagination, and error classification.

#include "GitHubProjectsV2Pure.h"
#include "SmatchetResult.h"
#include "TrackerError.h"

#include <map>
#include <string>
#include <vector>

namespace smatchet {
namespace github {

/// Fetch the configured project's identity + representable custom fields.
/// One GraphQL round-trip. `owner` is cfg.GitHubOwner (org or user login);
/// `projectNumber` is cfg.GitHubProjectNumber.
Result<ProjectV2Catalog, TrackerError> FetchProjectV2Catalog(const std::string& baseUrl, const std::string& pat,
                                                             const std::string& owner, int projectNumber);

/// Fetch every item's field values for the project, keyed by ticket key
/// ("owner/repo#N"). Paginated at 100/page with a 5-page soft cap (~500 items);
/// `outWarning` is set when the cap truncates. `catalogFields` maps field node
/// ids to the grid's "pv2.<slug>" ids.
Result<std::map<std::string, std::vector<std::pair<std::string, std::string>>>, TrackerError>
FetchProjectV2ItemValues(const std::string& baseUrl, const std::string& pat, const std::string& projectNodeId,
                         const std::vector<TrackerField>& catalogFields, std::string* outWarning);

/// Resolve the project item id for one issue/PR. Ok("") when the issue is not
/// an item of the project (the caller surfaces a targeted error).
Result<std::string, TrackerError> ResolveProjectV2ItemId(const std::string& baseUrl, const std::string& pat,
                                                         const std::string& owner, const std::string& repo,
                                                         long long issueNumber, const std::string& projectNodeId);

/// Apply one field edit: `valueOrNull` non-null → updateProjectV2ItemFieldValue;
/// null → clearProjectV2ItemFieldValue. Returns Ok() on success.
TrackerError UpdateProjectV2FieldValue(const std::string& baseUrl, const std::string& pat,
                                       const std::string& projectNodeId, const std::string& itemNodeId,
                                       const std::string& fieldNodeId, const nlohmann::json& valueOrNull);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_PROJECTS_V2_H
