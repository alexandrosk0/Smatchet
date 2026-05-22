#ifndef SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H
#define SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H

// PR12 of docs/design/github-tracker-backend.md — pure-logic JSON → CachedTicket
// mapping helpers extracted out of GitHubIssueSearch.cpp (which pulls cpr) so
// the doctest rig can exercise them without HTTP.
//
// Two surfaces:
//   - MapIssueOrPullRequestJsonToCachedTicket: maps a single `/issues` or
//     `/search/issues` item to a CachedTicket. Detects PR shape via the
//     `pull_request` sub-object, applies the `[PR] ` summary prefix + status
//     merge encoding (open / closed / merged-PR), and leaves PR-only fields
//     empty (the caller enriches them via the per-PR /pulls/{n} fetch).
//   - EnrichPullRequestFieldsFromJson: populates `pr.head`, `pr.base`,
//     `pr.mergeable`, `pr.draft` from a `/repos/{o}/{r}/pulls/{n}` JSON
//     payload. `mergeable == null` (GitHub still computing) → "computing".

#include "CachedTicketTypes.h"

#include <nlohmann/json.hpp>

#include <string>

namespace smatchet {
namespace github {

/// Sentinel field id placed by MapIssueOrPullRequestJsonToCachedTicket on PR
/// rows. The fetch helper uses it to drive the per-PR enrichment loop, then
/// erases it before returning the result. Tests can read it to assert PR
/// detection without coupling to the `[PR] ` summary prefix.
constexpr const char* kIsPullRequestSentinel = "_smatchet_is_pr";

/// PR12 — entry point used by `FetchIssuesViaRestApi` + the doctest rig.
/// `ownerHint` / `repoHint` are used when the issue payload doesn't carry
/// `repository_url` (single-issue endpoint shape).
CachedTicket MapIssueOrPullRequestJsonToCachedTicket(const nlohmann::json& issue, const std::string& ownerHint,
                                                     const std::string& repoHint);

/// PR12 — enrich a ticket already mapped from a list/search response with the
/// 4 PR-only fields (pr.head, pr.base, pr.mergeable, pr.draft) extracted from
/// a per-PR `/repos/{o}/{r}/pulls/{n}` payload. Tolerant of missing/null
/// fields — leaves the corresponding entry as the empty string when the
/// source payload doesn't carry it.
void EnrichPullRequestFieldsFromJson(CachedTicket& ticket, const nlohmann::json& prDetail);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H
