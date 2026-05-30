#ifndef SMATCHET_GITHUB_FETCH_PLAN_H
#define SMATCHET_GITHUB_FETCH_PLAN_H

// PR4 follow-up of docs/plans/shipped/github-tracker-backend.md.
// Pure helper deciding which GitHub REST endpoint to hit (cross-repo
// /search/issues vs repo-scoped /repos/{o}/{r}/issues) and what `q=` to send,
// given the user's Preferences (Owner, Repo) + the JQL-translator output.
// Lives in its own TU so the bucket-A test rig can link it without dragging
// in cpr / LocalCacheManager (the full GitHubIssueSearch.cpp pulls HTTP).

#include <string>

namespace smatchet {
namespace github {

struct GitHubFetchPlan {
    /// true → caller should hit /repos/{owner}/{repo}/issues (effectiveQuery
    /// is ignored on that path because the endpoint doesn't accept q=).
    /// false → caller should hit /search/issues?q=<effectiveQuery>.
    bool repoScoped = false;

    /// The `q=` value the caller should send on the cross-repo path.
    /// Empty on the repo-scoped path. On the cross-repo fallback path
    /// (owner+repo+jql all empty) this is "is:issue is:open".
    std::string effectiveQuery;

    /// PR12 — true when the source JQL contained `type:pr`. Both endpoints
    /// need this signal: the repo-scoped path uses it to keep PR items in
    /// the response (the default filter drops them), and the cross-repo
    /// path has already had `is:pr` injected into the effective query but
    /// still needs the flag so the per-PR enrichment loop (GET /pulls/{n})
    /// runs.
    bool includePullRequests = false;

    /// github-commit-tracker-rows — run the GraphQL issue/PR search. False only
    /// for a commits-only (`type:commit`) view. Default true preserves the
    /// existing issues/PRs path.
    bool includeIssuesOrPullRequests = true;

    /// github-commit-tracker-rows — fetch commit rows via REST
    /// `/repos/{o}/{r}/commits`. Requires owner+repo (cross-repo commit search
    /// is out of scope). Set for `type:commit` and `type:all`/`type:any`.
    bool includeCommits = false;

    /// Single human-readable warning string, or empty when the plan is
    /// unambiguous. The caller forwards this to outWarning so TicketSyncService
    /// surfaces it as a Sync Warning toast (5s).
    std::string warning;
};

/// Inputs:
///   owner / repo    — GitHub Owner + Repo from Preferences. Both set →
///                     repo-scoped. Both empty → cross-repo. Partial config
///                     (one set, one empty) → cross-repo + a partial-config
///                     warning so the user understands why their config
///                     didn't scope the search.
///   translatedQuery — the JQL → GitHub-search translator's output Query
///                     field (may be empty).
///   includePullRequests — PR12 / github-commit-tracker-rows: whether PR rows
///                     are kept in the node mapping (translator
///                     `IncludePullRequests`). Forwarded onto
///                     `plan.includePullRequests` unchanged.
///   includeCommits    — github-commit-tracker-rows: fetch commit rows via
///                     REST. When owner OR repo is empty, the plan downgrades
///                     this to false + emits a warning (cross-repo commit
///                     search is out of scope).
///   includeIssuesOrPullRequests — github-commit-tracker-rows: run the GraphQL
///                     issue/PR search. False only for commits-only views.
GitHubFetchPlan ComputeGitHubFetchPlan(const std::string& owner, const std::string& repo,
                                       const std::string& translatedQuery, bool includePullRequests = false,
                                       bool includeCommits = false, bool includeIssuesOrPullRequests = true);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_FETCH_PLAN_H
