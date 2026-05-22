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
                                       const std::string& translatedQuery, bool isPullRequestQuery) {
    GitHubFetchPlan plan;
    plan.includePullRequests = isPullRequestQuery;
    const bool ownerSet = !owner.empty();
    const bool repoSet = !repo.empty();

    if (ownerSet && repoSet) {
        plan.repoScoped = true;
        return plan;
    }

    if (ownerSet != repoSet) {
        plan.warning = "GitHub fetch: Owner='" + owner + "' Repo='" + repo +
                       "' partial config — falling back to cross-repo /search/issues. "
                       "Set both Owner+Repo in Preferences to scope to a single repository.";
    }

    if (translatedQuery.empty()) {
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
    } else {
        plan.effectiveQuery = translatedQuery;
    }
    return plan;
}

} // namespace github
} // namespace smatchet
