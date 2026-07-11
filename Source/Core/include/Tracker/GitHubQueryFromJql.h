#ifndef SMATCHET_GITHUB_QUERY_FROM_JQL_H
#define SMATCHET_GITHUB_QUERY_FROM_JQL_H

#include <string>

// docs/plans/shipped/github-tracker-backend.md — pure-helper JQL → GitHub
// /search/issues `q=` translator. Pure logic, no I/O, no globals. Lives in
// its own TU so the doctest rig links it without pulling cpr/SQLite/ImGui
// (mirrors the LabelEditDiffPure / GitHubClientHelpers convention).
// Users keep authoring view queries in JQL syntax; this helper converts the
// supported subset to GitHub search-qualifier syntax at fetch time. Unknown
// terms are dropped with a Warning rather than failing translation — the
// caller decides whether to surface the warning to the UI.

namespace smatchet {
namespace github {

struct JqlToGitHubResult {
    /// The translated GitHub search query string (the value passed to `?q=`).
    /// Empty when input was empty or every supported term was dropped; in that
    /// case the caller typically falls back to a repo-scoped is:open listing.
    std::string Query;
    /// Non-fatal: human-readable description of unsupported / dropped JQL
    /// terms. Empty when the full input translated cleanly. Multiple warnings
    /// are concatenated with "; " separators.
    std::string Warning;
    /// True on successful translation (including the empty-input case).
    /// False only on genuine parse failure where Query is unusable.
    bool Ok = false;
    /// Populated when Ok == false.
    std::string Error;
    /// True when the source JQL contained `type:pr` (or `type = "pr"`).
    /// Caller threads this through `ComputeGitHubFetchPlan` so the
    /// repo-scoped path knows to keep PR items instead of filtering them out,
    /// and so the cross-repo path knows it has already injected `is:pr` into
    /// the body. Default `false` preserves issues-only behavior.
    bool IsPullRequestQuery = false;
    /// github-commit-tracker-rows — true when PR rows should be KEPT in the
    /// node mapping. Set for `type:pr` (PR-only) AND for `type:all`/`type:any`
    /// (issues + PRs + commits). Distinct from `IsPullRequestQuery`, which also
    /// drives the `is:pr` query injection that would exclude plain issues.
    bool IncludePullRequests = false;
    /// github-commit-tracker-rows — true when the GraphQL issue/PR search
    /// should run. False only for `type:commit` (commits-only). Default true
    /// preserves the issues/PRs path.
    bool IncludeIssuesOrPullRequests = true;
    /// github-commit-tracker-rows — true when commit rows should be fetched via
    /// the REST `/repos/{o}/{r}/commits` path. Set for `type:commit` and
    /// `type:all`/`type:any`. Default false preserves issues-only behavior.
    bool IncludeCommits = false;
    /// user-info-window item 18c — canonical `owner/repo#N` key when the JQL
    /// carried a `key = "<owner>/<repo>#<N>"` clause. The fetch path post-filters
    /// results to this single issue (GitHub search has no exact issue-key
    /// qualifier). Empty when no key clause was present.
    std::string KeyFilter;
};

/// Translate a Smatchet JQL view query to a GitHub /search/issues `q=` value.
/// `owner` / `repo` are optional context: when both are non-empty the result
/// query is prefixed with `repo:<owner>/<repo>`. The cross-repo path passes
/// empty strings and relies on JQL anchors to scope the search.
/// Supported JQL terms (operator + keyword are case-insensitive):
///   project = <KEY>            → ignored with warning
///   assignee = currentUser()   → assignee:@me
///   assignee = "<login>"       → assignee:<login>
///   status = "Open"            → is:open
///   status = "Closed"          → is:closed
///   status != "Closed"         → is:open
///   labels = "<label>"         → label:"<label>" (chained via space = AND)
///   text ~ "<phrase>"          → "<phrase>"
///   reporter = currentUser()   → author:@me
///   type = "pr" / type:pr      → is:pr (sets result.IsPullRequestQuery=true)
///   type = "issue" / type:issue → is:issue (explicit; GitHub default)
///   type = "commit" / type:commit → commit rows only (IncludeCommits=true,
///                                IncludeIssuesOrPullRequests=false; no query term)
///   type:all / type:any        → issues + PRs + commits (IncludePullRequests
///                                + IncludeCommits true; no is:pr/is:issue term)
///   key = "owner/repo#N"       → result.KeyFilter (client-side post-filter;
///                                non-parsing values drop with a warning)
///   ORDER BY <field> <dir>     → ignored with warning
///   AND / OR connectors        → AND becomes space (GitHub default);
///                                OR is unsupported and emits a warning
/// Unsupported / unknown terms drop with a warning. Empty input → Ok=true,
/// Query empty (or just `repo:o/r` when owner+repo provided).
JqlToGitHubResult TranslateJqlToGitHubSearch(const std::string& jql, const std::string& owner, const std::string& repo);

/// Extract the leading `owner/repo` project anchor from a tracker query string.
/// Returns the token only when the FIRST whitespace-delimited token is itself
/// shaped `owner/repo` (a single '/', GitHub owner/repo alphabet on both sides).
/// A stray '/' elsewhere in the query (e.g. a `ui/ux` label value) yields "".
std::string ExtractGitHubProjectAnchor(const std::string& query);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_QUERY_FROM_JQL_H
