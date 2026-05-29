#ifndef SMATCHET_GITHUB_CLIENT_HELPERS_H
#define SMATCHET_GITHUB_CLIENT_HELPERS_H

#include <cstdint>
#include <string>

// GitHubClientHelpers — pure helpers consumed by GitHubClient. Slice 1 of
// docs/plans/shipped/github-tracker-backend.md PR2. Mirrors the JiraIssueSearch /
// PlaneFieldCatalog split convention (pure helpers live in their own TU so
// the doctest rig can link them without dragging in cpr / SQLite / ImGui).

namespace smatchet {
namespace github {

/// Parsed `owner/repo#N` issue key. Fields are populated only on success;
/// callers must check the return value of `ParseGitHubIssueKey` before reading.
struct ParsedIssueKey {
    std::string Owner;
    std::string Repo;
    std::int64_t Number = 0;
};

/// Parse a canonical GitHub issue key of the form `owner/repo#N` (e.g.
/// `alexandrosk0/Smatchet#42`). Strict — exactly one `/` separates owner from
/// repo, exactly one `#` separates repo from number, the number is a positive
/// integer. Returns false + leaves `out` unchanged on any mismatch.
/// Used by `ITrackerIssueMutations::UpdateField` + every URL-building path; the
/// CONTEXT.md glossary § TrackerIssueKey cross-link is the contract anchor.
bool ParseGitHubIssueKey(const std::string& issueKey, ParsedIssueKey& out);

/// Build the URL path suffix (no host, no `/repos` prefix) for the
/// "list issues" endpoint. `perPage` is clamped to [1, 100] per GitHub's API
/// docs; `sinceUnixSec` formats as ISO-8601 when non-zero, omitted otherwise.
/// Returns a string like `/repos/{owner}/{repo}/issues?per_page=30&since=2024-01-01T00:00:00Z`.
std::string BuildIssueListUrlSuffix(const std::string& owner, const std::string& repo, int perPage,
                                    std::int64_t sinceUnixSec);

/// github-commit-tracker-rows — parsed `owner/repo@<sha>` commit key. Fields
/// are populated only on success; callers must check the return value of
/// `ParseGitHubCommitKey` before reading. The `@` separator distinguishes a
/// commit key from the `owner/repo#N` issue key shape so a single string id
/// can be routed to the correct browse / mutation path.
struct ParsedCommitKey {
    std::string Owner;
    std::string Repo;
    std::string Sha;
};

/// github-commit-tracker-rows — parse a GitHub commit key of the form
/// `owner/repo@<sha>` (e.g. `alexandrosk0/Smatchet@a1b2c3d...`). Strict —
/// exactly one `/` separates owner from repo, exactly one `@` separates repo
/// from the SHA, owner/repo use the same charset as issue keys, and the SHA is
/// 7–40 lower/upper hex chars (abbreviated through full git object id).
/// Returns false + leaves `out` unchanged on any mismatch.
bool ParseGitHubCommitKey(const std::string& commitKey, ParsedCommitKey& out);

/// github-commit-tracker-rows — build the URL path suffix for the "list
/// commits" endpoint. `perPage` clamped to [1, 100]. Returns a string like
/// `/repos/{owner}/{repo}/commits?per_page=100`. Slice 1 fetches the most
/// recent page of the repository default branch (no `sha`/`since` window).
std::string BuildCommitListUrlSuffix(const std::string& owner, const std::string& repo, int perPage);

/// github-commit-tracker-rows — build the browse-path suffix (no host) for a
/// commit, e.g. `/{owner}/{repo}/commit/{sha}`. `GitHubClient::BuildBrowseUrl`
/// prepends the host (api.github.com → github.com, or the Enterprise host with
/// the `/api/v3` suffix stripped).
std::string BuildCommitBrowseUrlSuffix(const std::string& owner, const std::string& repo, const std::string& sha);

/// Build the URL path suffix for PATCH on a single issue (title/body/state/
/// milestone updates). Caller composes the JSON body; this only handles the URL.
std::string BuildIssuePatchUrlSuffix(const std::string& owner, const std::string& repo, std::int64_t number);

/// Validate a GitHub base URL — accepts `https://api.github.com` (cloud) or
/// `https://<host>/api/v3` (GitHub Enterprise). Rejects empty / non-https /
/// trailing-slash inconsistencies. Returns false + sets `outError` on reject.
bool IsValidGitHubBaseUrl(const std::string& baseUrl, std::string& outError);

/// Best-effort extract a human-readable error message from a GitHub error JSON
/// payload (e.g. `{"message": "Not Found", "documentation_url": "..."}`).
/// Falls back to `"HTTP <status>"` when the payload doesn't include a `message`
/// field. Used by every write path to produce the audit-trail's `errorMessage`.
std::string ExtractGitHubErrorMessage(int httpStatus, const std::string& body);

/// Parse an ISO-8601 timestamp ("2024-01-15T12:34:56Z") into unix epoch seconds.
/// Returns 0 + sets `outError` on parse failure. Permissive on offset format
/// (accepts `+00:00`, `Z`, missing offset = UTC). Used by FetchIssues to
/// populate `CachedTicket::CreatedAtSec` + `UpdatedAtSec`.
std::int64_t ParseIso8601ToUnixSec(const std::string& iso8601, std::string& outError);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_CLIENT_HELPERS_H
