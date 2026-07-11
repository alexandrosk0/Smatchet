#ifndef SMATCHET_LINEAR_ISSUE_SEARCH_H
#define SMATCHET_LINEAR_ISSUE_SEARCH_H

// Private header — declarations for the Linear issue-fetch helpers used by
// LinearClient::FetchIssues / FetchIssuesStreamed / FetchIssuesForKeys. Not part
// of the public Source/Core/include surface; lives next to its implementation so
// only LinearClient.cpp + LinearIssueSearch.cpp pick it up.
// Slice 2 of docs/plans/linear-tracker-backend.md — mirrors the GitHub
// GitHubIssueSearch.h thin-adapter/pure-TU split.

#include "CachedTicketTypes.h"
#include "SmatchetResult.h"
#include "TrackerError.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace smatchet {
namespace linear {

/// Build the Authorization + Content-Type header set used by every Linear
/// GraphQL POST. Linear personal API keys are sent RAW (no `Bearer ` prefix —
/// `BuildLinearAuthHeaderValue` pins that contract); the body is always JSON.
/// Defined in LinearIssueSearch.cpp, declared here so LinearClient.cpp shares
/// the helper without copying it (mirrors GitHub's `BuildGitHubHeaders`).
cpr::Header BuildLinearHeaders(const std::string& apiKey);

/// Slice 2 — cursor-paginated `issues` fetch used by LinearClient::FetchIssues
/// and ::FetchIssuesStreamed. POSTs the GraphQL `issues(filter,first,after)`
/// query to `apiUrl`, building the `filter` variable by merging the team scope
/// (`teamKey` preferred, else `teamId`) with the JQL-subset translation of
/// `viewQuery`. Paginates via `pageInfo.endCursor` up to a soft cap, warning at
/// the cap rather than failing. Each page's mapped tickets are appended to the
/// returned vector AND emitted via `onPage(page, isLast)` (when non-null) as
/// soon as the page maps, so the grid populates progressively.
///   - `*outFullSyncCompleted` is true iff the loop exited because the server
///     reported `hasNextPage == false` (no cap / fatal-error short-circuit).
///   - `*outFetchError` is set on a fatal error (HTTP failure, JSON parse,
///     GraphQL `errors[]`).
///   - `*outWarning` is set on non-fatal degradations (cap hit, translator
///     warnings).
/// `onPage` fires `isLast == true` exactly once for any non-empty fetch (zero
/// pages → no callback invocations); a terminal empty page is emitted on a
/// fatal/zero-source path so the stream always closes.
// SMATCHET_DEVIATION(rule=duplication; reason=backend API symmetry; owner=tracker; revisit=2026-12-31)
std::vector<CachedTicket>
FetchIssuesViaGraphQl(const std::string& apiUrl, const std::string& apiKey, const std::string& teamId,
                      const std::string& teamKey, const std::string& viewQuery, bool* outFullSyncCompleted,
                      std::string* outFetchError, std::string* outWarning,
                      /* `outFetchErrorStructured` (below, optional): classified twin of outFetchError
                         (retire-transport-error-text item 12), filled at the same fatal branch. */
                      const std::function<void(const std::vector<CachedTicket>& page, bool isLast)>& onPage,
                      TrackerError* outFetchErrorStructured = nullptr);

/// Slice 2 — per-key lookup used by LinearClient::FetchIssuesForKeys. Parses each
/// `TEAM-123` key into {teamKey, number}, builds a single `issues` query with an
/// `or:[{team,number}, ...]` filter, and maps the result. Stops at the first
/// failure and returns Err carrying the diagnostic; Ok carries the full list on
/// success (mirrors GitHub's FetchIssuesForKeysViaRestApi).
Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeysViaGraphQl(const std::string& apiUrl,
                                                                             const std::string& apiKey,
                                                                             const std::vector<std::string>& issueKeys);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_ISSUE_SEARCH_H
