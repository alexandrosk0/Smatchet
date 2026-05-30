#include "GitHubFetchPlan.h"

#include <string>

namespace smatchet {
namespace github {

namespace {

// Mirrors the pagination cap in GitHubIssueSearch.cpp. Kept private here so
// the warning message can quote the numeric bound without coupling the test
// rig to the HTTP-side constants.
constexpr int kPlanPerPage = 100;
constexpr int kPlanMaxPages = 10;

} // namespace

GitHubFetchPlan ComputeGitHubFetchPlan(const std::string& owner, const std::string& repo,
                                       const std::string& translatedQuery, bool includePullRequests,
                                       bool includeCommits, bool includeIssuesOrPullRequests) {
    GitHubFetchPlan plan;
    plan.includePullRequests = includePullRequests;
    plan.includeIssuesOrPullRequests = includeIssuesOrPullRequests;
    plan.includeCommits = includeCommits;
    const bool ownerSet = !owner.empty();
    const bool repoSet = !repo.empty();

    // github-commit-tracker-rows — commit fetch requires a scoped repo. Downgrade
    // + warn when commits were requested without owner+repo (cross-repo commit
    // search is out of scope for slice 1).
    if (includeCommits && !(ownerSet && repoSet)) {
        plan.includeCommits = false;
        plan.warning = "GitHub commit rows requested but Owner+Repo not both set — commit fetch skipped "
                       "(cross-repo commit search is out of scope). Set Owner+Repo in Preferences.";
    }

    if (ownerSet && repoSet) {
        plan.repoScoped = true;
        return plan;
    }

    // The partial-config + empty-query warnings below describe the issue/PR
    // /search/issues fallback, so they only apply when that path actually runs.
    // For a commits-only view (includeIssuesOrPullRequests=false) the
    // commit-downgrade warning above is the accurate message (CR #504).
    if (ownerSet != repoSet && includeIssuesOrPullRequests) {
        // Append (don't clobber) — a commit-downgrade warning may already be set
        // above when commits were requested under this same partial config.
        const std::string partial = "GitHub fetch: Owner='" + owner + "' Repo='" + repo +
                                    "' partial config — falling back to cross-repo /search/issues. "
                                    "Set both Owner+Repo in Preferences to scope to a single repository.";
        if (plan.warning.empty()) {
            plan.warning = partial;
        } else {
            plan.warning += " " + partial;
        }
    }

    if (translatedQuery.empty()) {
        if (includeIssuesOrPullRequests) {
            plan.effectiveQuery = "is:issue is:open";
            std::string extra = "No GitHub Owner/Repo and no view JQL — fetching newest open "
                                "issues globally (capped at " +
                                std::to_string(kPlanMaxPages * kPlanPerPage) +
                                "). Set Owner+Repo in Preferences or add a view JQL filter "
                                "for relevant results.";
            if (plan.warning.empty()) {
                plan.warning = std::move(extra);
            } else {
                plan.warning += " " + extra;
            }
        }
    } else {
        plan.effectiveQuery = translatedQuery;
    }
    return plan;
}

} // namespace github
} // namespace smatchet
