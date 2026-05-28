// GitHubQueryFromJql doctest — JQL → GitHub /search/issues `q=` translator.
// PR5 of docs/design/github-tracker-backend.md.

#include "GitHubQueryFromJql.h"

#include <doctest/doctest.h>

#include <string>

using namespace smatchet::github;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("TranslateJqlToGitHubSearch — empty input, no owner/repo") {
    const auto r = TranslateJqlToGitHubSearch("", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — empty input with owner/repo emits repo: prefix") {
    const auto r = TranslateJqlToGitHubSearch("", "alexandrosk0", "Smatchet");
    CHECK(r.Ok);
    CHECK(r.Query == "repo:alexandrosk0/Smatchet");
}

TEST_CASE("TranslateJqlToGitHubSearch — assignee = currentUser() → assignee:@me") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser()", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "assignee:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — assignee = \"octocat\" → assignee:octocat") {
    const auto r = TranslateJqlToGitHubSearch("assignee = \"octocat\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "assignee:octocat");
}

TEST_CASE("TranslateJqlToGitHubSearch — AND chain assignee + status") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser() AND status = \"Open\"", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "assignee:@me"));
    CHECK(Contains(r.Query, "is:open"));
}

TEST_CASE("TranslateJqlToGitHubSearch — project = SMAT → warning, no project: term") {
    const auto r = TranslateJqlToGitHubSearch("project = SMAT", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK_FALSE(r.Warning.empty());
    CHECK(Contains(r.Warning, "project"));
}

TEST_CASE("TranslateJqlToGitHubSearch — status != \"Closed\" → is:open") {
    const auto r = TranslateJqlToGitHubSearch("status != \"Closed\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "is:open");
}

TEST_CASE("TranslateJqlToGitHubSearch — labels = \"bug\" → label:bug") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"bug\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "label:bug");
}

TEST_CASE("TranslateJqlToGitHubSearch — text ~ \"memory leak\" → quoted phrase") {
    const auto r = TranslateJqlToGitHubSearch("text ~ \"memory leak\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "\"memory leak\"");
}

TEST_CASE("TranslateJqlToGitHubSearch — reporter = currentUser() → author:@me") {
    const auto r = TranslateJqlToGitHubSearch("reporter = currentUser()", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "author:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — owner+repo provided prefixes repo:") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser()", "alexandrosk0", "Smatchet");
    CHECK(r.Ok);
    CHECK(r.Query == "repo:alexandrosk0/Smatchet assignee:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — ORDER BY clause dropped with warning") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser() ORDER BY created DESC", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "assignee:@me");
    CHECK(Contains(r.Warning, "ORDER BY"));
}

TEST_CASE("TranslateJqlToGitHubSearch — ORDER BY substring inside quoted text NOT stripped") {
    // Regression for CR finding on PR #387: pre-tokenize lower.find("order by")
    // was context-blind and would corrupt the quoted phrase. Token-aware
    // detection only matches bare Word tokens.
    const auto r = TranslateJqlToGitHubSearch("text ~ \"work order by priority\"", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "\"work order by priority\""));
    CHECK_FALSE(Contains(r.Warning, "ORDER BY"));
}

TEST_CASE("TranslateJqlToGitHubSearch — OR connector emits warning, terms still emitted as AND") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser() OR status = \"Open\"", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "assignee:@me"));
    CHECK(Contains(r.Query, "is:open"));
    CHECK(Contains(r.Warning, "OR"));
}

TEST_CASE("TranslateJqlToGitHubSearch — multi-word label gets quoted") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"needs review\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "label:\"needs review\"");
}

TEST_CASE("TranslateJqlToGitHubSearch — extra whitespace doesn't break") {
    const auto r = TranslateJqlToGitHubSearch("   assignee   =   currentUser()   ", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "assignee:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — case-insensitive keywords + field names") {
    const auto r = TranslateJqlToGitHubSearch("ASSIGNEE = CURRENTUSER()", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "assignee:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — owner+repo without JQL terms") {
    const auto r = TranslateJqlToGitHubSearch("", "alexandrosk0", "Smatchet");
    CHECK(r.Ok);
    CHECK(r.Query == "repo:alexandrosk0/Smatchet");
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — chained labels via AND") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"bug\" AND labels = \"p1\"", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "label:bug"));
    CHECK(Contains(r.Query, "label:p1"));
}

TEST_CASE("TranslateJqlToGitHubSearch — unsupported field emits warning") {
    const auto r = TranslateJqlToGitHubSearch("sprint = 42", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "sprint"));
}

// PR12 — `type:` token recognition.

TEST_CASE("TranslateJqlToGitHubSearch — type:pr sets is:pr + IsPullRequestQuery") {
    const auto r = TranslateJqlToGitHubSearch("type:pr", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(r.IsPullRequestQuery == true);
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — type:issue emits is:issue without IsPullRequestQuery") {
    const auto r = TranslateJqlToGitHubSearch("type:issue", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:issue"));
    CHECK(r.IsPullRequestQuery == false);
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — type:pr AND assignee = currentUser()") {
    const auto r = TranslateJqlToGitHubSearch("type:pr AND assignee = currentUser()", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(Contains(r.Query, "assignee:@me"));
    CHECK(r.IsPullRequestQuery == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — TYPE:PR case-insensitive") {
    const auto r = TranslateJqlToGitHubSearch("TYPE:PR", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(r.IsPullRequestQuery == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — no type token preserves issues-only default") {
    const auto r = TranslateJqlToGitHubSearch("assignee = currentUser()", "", "");
    CHECK(r.Ok);
    CHECK_FALSE(Contains(r.Query, "is:pr"));
    CHECK_FALSE(Contains(r.Query, "is:issue"));
    CHECK(r.IsPullRequestQuery == false);
}

TEST_CASE("TranslateJqlToGitHubSearch — type:foo unknown value drops with warning") {
    const auto r = TranslateJqlToGitHubSearch("type:foo", "", "");
    CHECK(r.Ok);
    CHECK_FALSE(Contains(r.Query, "is:pr"));
    CHECK_FALSE(Contains(r.Query, "is:issue"));
    CHECK(r.IsPullRequestQuery == false);
    CHECK(Contains(r.Warning, "type:"));
    CHECK(Contains(r.Warning, "foo"));
}

TEST_CASE("TranslateJqlToGitHubSearch — type = \"pr\" full JQL shape") {
    const auto r = TranslateJqlToGitHubSearch("type = \"pr\"", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(r.IsPullRequestQuery == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — type:pr also sets IncludePullRequests") {
    const auto r = TranslateJqlToGitHubSearch("type:pr", "", "");
    CHECK(r.IncludePullRequests == true);
    CHECK(r.IncludeIssuesOrPullRequests == true);
    CHECK(r.IncludeCommits == false);
}

// github-commit-tracker-rows — type:commit / type:all / type:any.

TEST_CASE("TranslateJqlToGitHubSearch — type:commit selects commits only, no query term") {
    const auto r = TranslateJqlToGitHubSearch("type:commit", "alexandrosk0", "Smatchet");
    CHECK(r.Ok);
    CHECK(r.IncludeCommits == true);
    CHECK(r.IncludeIssuesOrPullRequests == false);
    CHECK(r.IncludePullRequests == false);
    CHECK(r.IsPullRequestQuery == false);
    // No is:commit qualifier exists; only the repo: prefix is emitted.
    CHECK(r.Query == "repo:alexandrosk0/Smatchet");
    CHECK_FALSE(Contains(r.Query, "is:commit"));
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — type = \"commit\" full JQL shape") {
    const auto r = TranslateJqlToGitHubSearch("type = \"commit\"", "", "");
    CHECK(r.Ok);
    CHECK(r.IncludeCommits == true);
    CHECK(r.IncludeIssuesOrPullRequests == false);
}

TEST_CASE("TranslateJqlToGitHubSearch — type:all enables issues+PRs+commits") {
    const auto r = TranslateJqlToGitHubSearch("type:all", "", "");
    CHECK(r.Ok);
    CHECK(r.IncludeCommits == true);
    CHECK(r.IncludePullRequests == true);
    CHECK(r.IncludeIssuesOrPullRequests == true);
    CHECK(r.IsPullRequestQuery == false); // no is:pr injection — issues stay too
    CHECK_FALSE(Contains(r.Query, "is:pr"));
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — type:any is an alias for type:all") {
    const auto r = TranslateJqlToGitHubSearch("type:any", "", "");
    CHECK(r.Ok);
    CHECK(r.IncludeCommits == true);
    CHECK(r.IncludePullRequests == true);
    CHECK(r.IncludeIssuesOrPullRequests == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — unknown type value warning lists commit + all") {
    const auto r = TranslateJqlToGitHubSearch("type:foo", "", "");
    CHECK(r.Ok);
    CHECK(r.IncludeCommits == false);
    CHECK(r.IncludeIssuesOrPullRequests == true);
    CHECK(Contains(r.Warning, "commit"));
    CHECK(Contains(r.Warning, "all"));
    CHECK(Contains(r.Warning, "any")); // CR #504 — accepted aliases listed
}
