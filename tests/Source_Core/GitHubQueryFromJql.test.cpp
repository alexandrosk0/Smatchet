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
