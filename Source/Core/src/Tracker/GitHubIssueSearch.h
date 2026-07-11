#ifndef SMATCHET_GITHUB_ISSUE_SEARCH_H
#define SMATCHET_GITHUB_ISSUE_SEARCH_H

// Private header — declarations for the GitHub issue-fetch helpers used by
// GitHubClient::FetchIssues / FetchIssuesForKeys. Not part of the public
// Source/Core/include surface; lives next to its implementation so only
// GitHubClient.cpp + GitHubIssueSearch.cpp pick it up.
// docs/plans/shipped/github-tracker-backend.md.

#include "CachedTicketTypes.h"
#include "SmatchetResult.h"
#include "TrackerError.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace smatchet {
namespace github {

/// Build the Authorization + Accept + API-version + User-Agent header set used
/// by every GitHub REST call. Mirrors the helper in the anon
/// namespace of GitHubClient.cpp — re-exposed here so GitHubIssueSearch.cpp
/// can reuse it without copying the implementation.
cpr::Header BuildGitHubHeaders(const std::string& pat);

/// Paginated fetch entry point used by GitHubClient::FetchIssues.
/// Two flow shapes selected by owner/repo presence:
///   - owner+repo non-empty → GET /repos/{owner}/{repo}/issues?state=all
///     (per_page=100, paginate by page=N). The repo-scoped REST endpoint
///     does not accept a `q=` search expression, so any non-empty JQL is
///     reported back via outWarning rather than applied server-side.
///   - both empty → GET /search/issues?q=<encoded translated JQL>&per_page=100
///     (paginate by page=N). The user is responsible for ensuring their JQL
///     scopes the search (cross-repo path).
/// Soft cap: 10 pages (≈1000 issues). When reached, surfaces a Warning and
/// returns the partial result rather than failing — matches the
/// PlaneIssueSearch / JiraIssueSearch convention.
/// `*outFullSyncCompleted` is true iff the loop exited because the server
/// returned a short page (last page) AND no fatal error or cap fired.
/// `*outFetchError` is set on a fatal error (HTTP failure, JSON parse).
/// `*outWarning` is set on non-fatal degradations (cap hit, repo-path JQL
/// drop, translator warnings).
std::vector<CachedTicket> FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat,
                                                const std::string& owner, const std::string& repo,
                                                const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                                                std::string* outFetchError, std::string* outWarning);

// SMATCHET_DEVIATION(rule=duplication; reason=backend API symmetry; owner=tracker; revisit=2026-12-31)
/// Latency fix — same as `FetchIssuesViaRestApi` but emits each page's
/// mapped tickets via `onPage` as soon as the page mapping completes (before
/// the next page's HTTP roundtrip). Used by `GitHubClient::FetchIssuesStreamed`
/// so the grid populates progressively (t+1.7s, t+3.4s, ...) instead of
/// all-at-once after the last page returns.
/// `onPage` is invoked with the mapped + sentinel-stripped tickets for that
/// page, and `isLast == true` on the final invocation (last page OR cap hit
/// OR fatal error mid-stream). `isLast` fires exactly once for any non-empty
/// fetch (zero pages → no callback invocations). When `onPage` is null the
/// helper accumulates into the return vector with no per-page emission,
/// matching the legacy single-shot contract.
/// Return value: same accumulated vector as the legacy overload (for callers
/// like single-shot tests that still want the all-at-once snapshot). The
/// streaming callers ignore the return value and consume the per-page batches.
/// `outFetchErrorStructured` (optional) receives the classified twin of `outFetchError`
/// (retire-transport-error-text item 12), filled at the same composition sites.
// SMATCHET_DEVIATION(rule=duplication; reason=backend API symmetry; owner=tracker; revisit=2026-12-31)
std::vector<CachedTicket>
FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat, const std::string& owner,
                      const std::string& repo, const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                      std::string* outFetchError, std::string* outWarning,
                      const std::function<void(const std::vector<CachedTicket>& page, bool isLast)>& onPage,
                      TrackerError* outFetchErrorStructured = nullptr);

/// Single-issue lookup loop used by GitHubClient::FetchIssuesForKeys.
/// For each key in `issueKeys` (canonical `owner/repo#N` shape), issues a
/// GET /repos/{owner}/{repo}/issues/{number}. Stops at the first failure
/// and returns Err carrying the diagnostic (any tickets appended before the
/// abort are dropped with the Err — Ok carries the full list on success).
Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeysViaRestApi(const std::string& baseUrl,
                                                                             const std::string& pat,
                                                                             const std::vector<std::string>& issueKeys);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_ISSUE_SEARCH_H
