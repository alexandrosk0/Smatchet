#ifndef SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H
#define SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H

// docs/plans/shipped/github-tracker-backend.md — pure-logic JSON → CachedTicket
// mapping helpers extracted out of GitHubIssueSearch.cpp (which pulls cpr) so
// the doctest rig can exercise them without HTTP.
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
#include <vector>

namespace smatchet {
namespace github {

/// Sentinel field id placed by MapIssueOrPullRequestJsonToCachedTicket on PR
/// rows. The fetch helper uses it to drive the per-PR enrichment loop, then
/// erases it before returning the result. Tests can read it to assert PR
/// detection without coupling to the `[PR] ` summary prefix.
constexpr const char* kIsPullRequestSentinel = "_smatchet_is_pr";

/// Display prefix the mapper prepends to pull-request summaries so users can
/// tell PRs apart at a glance. Shared with the write path
/// (GitHubMutationPure), which strips exactly one leading occurrence from a
/// summary edit so a PR title edit never compounds the prefix on round-trip.
constexpr const char* kPullRequestSummaryPrefix = "[PR] ";

/// Entry point used by `FetchIssuesViaRestApi` + the doctest rig.
/// `ownerHint` / `repoHint` are used when the issue payload doesn't carry
/// `repository_url` (single-issue endpoint shape).
/// github-commit-tracker-rows — also stamps `github.kind` = "issue" |
/// "pull_request" so the grid + commit-key router can tell row kinds apart.
CachedTicket MapIssueOrPullRequestJsonToCachedTicket(const nlohmann::json& issue, const std::string& ownerHint,
                                                     const std::string& repoHint);

/// github-commit-tracker-rows — map a single `/repos/{o}/{r}/commits` item to a
/// read-only commit `CachedTicket`. `owner` / `repo` come from the configured
/// repository (commit payloads don't carry `repository_url`). Tolerant of
/// missing nested `author` / `committer` / `verification` / `parents` per
/// Pillar 3 — a malformed field maps to a safe empty default rather than
/// throwing. Row shape (id `owner/repo@<sha>`, `github.kind`=commit, the
/// `commit.*` columns) is documented in
/// docs/plans/shipped/github-commit-tracker-rows.md § Row contract.
CachedTicket MapCommitJsonToCachedTicket(const nlohmann::json& commit, const std::string& owner,
                                         const std::string& repo);

/// Enrich a ticket already mapped from a list/search response with the
/// 4 PR-only fields (pr.head, pr.base, pr.mergeable, pr.draft) extracted from
/// a per-PR `/repos/{o}/{r}/pulls/{n}` payload. Tolerant of missing/null
/// fields — leaves the corresponding entry as the empty string when the
/// source payload doesn't carry it.
void EnrichPullRequestFieldsFromJson(CachedTicket& ticket, const nlohmann::json& prDetail);

/// Strategy C — adapt a GraphQL `search.nodes[i]` node (Issue or
/// PullRequest fragment) to the REST `/issues` JSON shape that
/// `MapIssueOrPullRequestJsonToCachedTicket` consumes. Keeps the existing
/// mapper pure-logic-tested instead of duplicating its branching for camelCase
/// + nested `author{login}` / `labels{nodes[]}`.
/// Inputs use GraphQL conventions:
///   - `__typename` is "Issue" or "PullRequest" (drives PR detection)
///   - `number`, `title`, `state` ("OPEN"/"CLOSED"/"MERGED"), `body`,
///     `createdAt`, `updatedAt`, `repository.{owner.login,name}`,
///     `author{login}`, `assignees.nodes[0].login`, `labels.nodes[].name`,
///     `milestone.title`
///   - PR-only: `headRefName`, `baseRefName`, `mergeable`
///     ("MERGEABLE"/"CONFLICTING"/"UNKNOWN"), `isDraft`, `mergedAt`
/// Output shape matches `/repos/{o}/{r}/issues` items: top-level `number`,
/// `title`, `state` ("open"/"closed"), `body`, `created_at`, `updated_at`,
/// `repository_url`, `user{login}`, `assignee{login}`, `labels[]{name}`,
/// `milestone{title}`, plus for PR nodes a `pull_request{url, merged_at}`
/// marker sub-object. The companion `MapGraphQlPullRequestNodeToRestPrShape`
/// returns the `/pulls/{n}` REST shape (`head.ref`, `base.ref`, `mergeable`,
/// `draft`) so `EnrichPullRequestFieldsFromJson` can consume it without
/// changes. The two mappers stay decoupled — the adapter is the only thing
/// that knows about GraphQL shape.
nlohmann::json MapGraphQlNodeToRestShape(const nlohmann::json& node);

/// Strategy C — adapt a GraphQL PullRequest node's PR-only fields
/// (`headRefName`, `baseRefName`, `mergeable`, `isDraft`) to the REST
/// `/pulls/{n}` JSON shape. Output is suitable for direct hand-off to
/// `EnrichPullRequestFieldsFromJson`. Returns `null` JSON when the node is
/// not a PR or is malformed (the caller treats null as "skip enrichment").
nlohmann::json MapGraphQlPullRequestNodeToRestPrShape(const nlohmann::json& node);

/// Latency fix — pure-logic helper exposed for doctest coverage. Maps a
/// GraphQL `search.nodes` array into a vector of CachedTicket, applying the
/// `includePullRequests` filter and stripping the `kIsPullRequestSentinel`
/// before return. Malformed nodes (mapping throws) are silently skipped so
/// the helper remains logger-free; the live caller in `GitHubIssueSearch.cpp`
/// can log a summary count if needed.
std::vector<CachedTicket> MapGraphQlNodesToTickets(const nlohmann::json& nodes, const std::string& owner,
                                                   const std::string& repo, bool includePullRequests);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_ISSUE_SEARCH_MAPPING_H
