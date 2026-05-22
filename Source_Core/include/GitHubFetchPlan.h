#ifndef SMATCHET_GITHUB_FETCH_PLAN_H
#define SMATCHET_GITHUB_FETCH_PLAN_H

// PR4 follow-up of docs/design/github-tracker-backend.md.
//
// Pure helper deciding which GitHub REST endpoint to hit (cross-repo
// /search/issues vs repo-scoped /repos/{o}/{r}/issues) and what `q=` to send,
// given the user's Preferences (Owner, Repo) + the JQL-translator output.
//
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
GitHubFetchPlan ComputeGitHubFetchPlan(const std::string& owner, const std::string& repo,
                                       const std::string& translatedQuery);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_FETCH_PLAN_H
