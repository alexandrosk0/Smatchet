// GitHubFetchPlan doctest — ComputeGitHubFetchPlan pure helper.
// PR4 follow-up of docs/design/github-tracker-backend.md.
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
