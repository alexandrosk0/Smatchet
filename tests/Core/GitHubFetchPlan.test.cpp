// GitHubFetchPlan doctest — ComputeGitHubFetchPlan pure helper.
// Part of docs/plans/shipped/github-tracker-backend.md.
//
// The helper decides whether to take the repo-scoped /repos/{o}/{r}/issues
// path or the cross-repo /search/issues path, and emits a single
// human-readable warning when the user's config + translator output are
// likely to produce surprising results (e.g. no filter at all → globally
// newest open issues across all 50M+ GitHub issues, capped at 1000).

#include "GitHubFetchPlan.h"

#include <doctest/doctest.h>

#include <string>

using namespace smatchet::github;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("ComputeGitHubFetchPlan — owner+repo set + non-empty query → repo-scoped, no warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", "assignee:@me");
    CHECK(plan.repoScoped == true);
    CHECK(plan.effectiveQuery.empty()); // repo-scoped path ignores q=
    CHECK(plan.warning.empty());
}

TEST_CASE("ComputeGitHubFetchPlan — owner+repo set + empty query → repo-scoped, no warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("octocat", "Hello-World", "");
    CHECK(plan.repoScoped == true);
    CHECK(plan.effectiveQuery.empty());
    CHECK(plan.warning.empty());
}

TEST_CASE("ComputeGitHubFetchPlan — both owner+repo empty + non-empty query → cross-repo, no warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("", "", "assignee:@me label:bug");
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "assignee:@me label:bug");
    CHECK(plan.warning.empty());
}

TEST_CASE("ComputeGitHubFetchPlan — both owner+repo empty + empty query → cross-repo fallback + warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("", "", "");
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "is:issue is:open");
    CHECK_FALSE(plan.warning.empty());
    CHECK(Contains(plan.warning, "No GitHub Owner/Repo"));
    CHECK(Contains(plan.warning, "1000")); // 10 pages × 100 items cap mentioned
}

TEST_CASE("ComputeGitHubFetchPlan — partial config: owner set, repo empty → cross-repo + partial-config warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "", "label:bug");
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "label:bug");
    CHECK_FALSE(plan.warning.empty());
    CHECK(Contains(plan.warning, "partial config"));
    CHECK(Contains(plan.warning, "alexandrosk0"));
}

TEST_CASE("ComputeGitHubFetchPlan — partial config: repo set, owner empty → cross-repo + partial-config warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("", "Smatchet", "is:open");
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "is:open");
    CHECK_FALSE(plan.warning.empty());
    CHECK(Contains(plan.warning, "partial config"));
    CHECK(Contains(plan.warning, "Smatchet"));
}

TEST_CASE("ComputeGitHubFetchPlan — partial config + empty query → combined warning") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "", "");
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "is:issue is:open");
    CHECK_FALSE(plan.warning.empty());
    // Both partial-config + fallback warnings should appear in one string.
    CHECK(Contains(plan.warning, "partial config"));
    CHECK(Contains(plan.warning, "No GitHub Owner/Repo"));
}

// `includePullRequests` propagation from translator output.

TEST_CASE("ComputeGitHubFetchPlan — repo-scoped + isPullRequestQuery=true keeps flag set") {
    const GitHubFetchPlan plan =
        ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", /*translatedQuery=*/"", /*isPullRequestQuery=*/true);
    CHECK(plan.repoScoped == true);
    CHECK(plan.effectiveQuery.empty());
    CHECK(plan.includePullRequests == true);
}

TEST_CASE("ComputeGitHubFetchPlan — cross-repo + isPullRequestQuery=true keeps flag set") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("", "", "assignee:@me is:pr", /*isPullRequestQuery=*/true);
    CHECK(plan.repoScoped == false);
    CHECK(plan.effectiveQuery == "assignee:@me is:pr");
    CHECK(plan.includePullRequests == true);
}

TEST_CASE("ComputeGitHubFetchPlan — default isPullRequestQuery=false preserves issues-only path") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", "");
    CHECK(plan.includePullRequests == false);
}

// github-commit-tracker-rows — row-source selection.

TEST_CASE("ComputeGitHubFetchPlan — defaults: issues-only, no commits") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", "");
    CHECK(plan.includeIssuesOrPullRequests == true);
    CHECK(plan.includeCommits == false);
}

TEST_CASE("ComputeGitHubFetchPlan — commits-only: includeCommits set, issues path off") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", "",
                                                        /*includePullRequests=*/false, /*includeCommits=*/true,
                                                        /*includeIssuesOrPullRequests=*/false);
    CHECK(plan.repoScoped == true);
    CHECK(plan.includeCommits == true);
    CHECK(plan.includeIssuesOrPullRequests == false);
    CHECK(plan.warning.empty());
}

TEST_CASE("ComputeGitHubFetchPlan — all sources: issues+PRs+commits enabled") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "Smatchet", "",
                                                        /*includePullRequests=*/true, /*includeCommits=*/true,
                                                        /*includeIssuesOrPullRequests=*/true);
    CHECK(plan.includePullRequests == true);
    CHECK(plan.includeCommits == true);
    CHECK(plan.includeIssuesOrPullRequests == true);
    CHECK(plan.warning.empty());
}

TEST_CASE("ComputeGitHubFetchPlan — commits requested without owner+repo downgrades + warns") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("", "", "",
                                                        /*includePullRequests=*/false, /*includeCommits=*/true,
                                                        /*includeIssuesOrPullRequests=*/false);
    CHECK(plan.includeCommits == false); // downgraded
    CHECK_FALSE(plan.warning.empty());
    CHECK(Contains(plan.warning, "commit"));
}

TEST_CASE("ComputeGitHubFetchPlan — commits requested with partial config (owner only) downgrades") {
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "", "",
                                                        /*includePullRequests=*/false, /*includeCommits=*/true,
                                                        /*includeIssuesOrPullRequests=*/true);
    CHECK(plan.includeCommits == false);
    CHECK(Contains(plan.warning, "commit"));
}

TEST_CASE("ComputeGitHubFetchPlan — commits-only partial config omits issue-search wording (CR #504)") {
    // includeIssuesOrPullRequests=false → the /search/issues fallback warnings
    // must not appear; only the commit-downgrade message is accurate.
    const GitHubFetchPlan plan = ComputeGitHubFetchPlan("alexandrosk0", "", "",
                                                        /*includePullRequests=*/false, /*includeCommits=*/true,
                                                        /*includeIssuesOrPullRequests=*/false);
    CHECK(plan.includeCommits == false);
    CHECK(Contains(plan.warning, "commit"));
    CHECK_FALSE(Contains(plan.warning, "/search/issues"));
    CHECK_FALSE(Contains(plan.warning, "newest open"));
}
