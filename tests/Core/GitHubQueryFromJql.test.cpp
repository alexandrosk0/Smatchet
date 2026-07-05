// GitHubQueryFromJql doctest — JQL → GitHub /search/issues `q=` translator.
// Part of docs/plans/shipped/github-tracker-backend.md.

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
    // Regression for a CR finding: pre-tokenize lower.find("order by")
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

// `type:` token recognition.

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

// #984 — native GitHub `is:` qualifier passthrough sets the matching include-flags.
// Before the fix, raw `is:pr` returned PRs from GraphQL but the row-mapper dropped
// every PR because IncludePullRequests stayed false (silent 0-row sync).

TEST_CASE("TranslateJqlToGitHubSearch — raw is:pr sets IncludePullRequests + re-emits the qualifier (#984)") {
    const auto r = TranslateJqlToGitHubSearch("is:pr", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(r.IncludePullRequests == true);
    CHECK(r.IsPullRequestQuery == true);
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — raw is:pr is:open keeps PRs and emits both qualifiers (#984)") {
    const auto r = TranslateJqlToGitHubSearch("is:pr is:open", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(Contains(r.Query, "is:open"));
    CHECK(r.IncludePullRequests == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — IS:PR case-insensitive (#984)") {
    const auto r = TranslateJqlToGitHubSearch("IS:PR", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:pr"));
    CHECK(r.IncludePullRequests == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — raw is:issue emits is:issue without IncludePullRequests (#984)") {
    const auto r = TranslateJqlToGitHubSearch("is:issue", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:issue"));
    CHECK(r.IncludePullRequests == false);
    CHECK(r.IsPullRequestQuery == false);
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — raw is:open / is:closed emit the qualifier, no PR flag (#984)") {
    const auto open = TranslateJqlToGitHubSearch("is:open", "", "");
    CHECK(open.Ok);
    CHECK(Contains(open.Query, "is:open"));
    CHECK(open.IncludePullRequests == false);

    const auto closed = TranslateJqlToGitHubSearch("is:closed", "", "");
    CHECK(closed.Ok);
    CHECK(Contains(closed.Query, "is:closed"));
    CHECK(closed.IncludePullRequests == false);
}

TEST_CASE("TranslateJqlToGitHubSearch — is:merged implies PRs (#984)") {
    const auto r = TranslateJqlToGitHubSearch("is:merged", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "is:merged"));
    CHECK(r.IncludePullRequests == true);
}

TEST_CASE("TranslateJqlToGitHubSearch — unknown is: qualifier warns, no flag (#984)") {
    const auto r = TranslateJqlToGitHubSearch("is:bogus", "", "");
    CHECK(r.Ok);
    CHECK_FALSE(Contains(r.Query, "is:bogus"));
    CHECK(r.IncludePullRequests == false);
    CHECK(Contains(r.Warning, "is:"));
    CHECK(Contains(r.Warning, "bogus"));
}

// PR A1 (full-function-size-compliance) — additional clause-dispatch coverage
// locking byte-identical behaviour across the StripOrderByClause /
// TranslateClauses / HandleFieldClause / HandleStatusClause / HandleTypeClause /
// ComposeQuery decomposition.

TEST_CASE("TranslateJqlToGitHubSearch — status = \"Open\" → is:open") {
    const auto r = TranslateJqlToGitHubSearch("status = \"Open\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "is:open");
}

TEST_CASE("TranslateJqlToGitHubSearch — status = \"Closed\" → is:closed") {
    const auto r = TranslateJqlToGitHubSearch("status = \"Closed\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "is:closed");
}

TEST_CASE("TranslateJqlToGitHubSearch — status != \"Open\" → is:closed") {
    const auto r = TranslateJqlToGitHubSearch("status != \"Open\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "is:closed");
}

TEST_CASE("TranslateJqlToGitHubSearch — status with no GitHub equivalent dropped with warning") {
    const auto r = TranslateJqlToGitHubSearch("status = \"Backlog\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Backlog"));
}

TEST_CASE("TranslateJqlToGitHubSearch — assignee != value drops as unsupported negation") {
    const auto r = TranslateJqlToGitHubSearch("assignee != \"octocat\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "negation"));
}

TEST_CASE("TranslateJqlToGitHubSearch — unsupported operator ~ on assignee warns") {
    const auto r = TranslateJqlToGitHubSearch("assignee ~ \"octo\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Unsupported operator"));
    CHECK(Contains(r.Warning, "assignee"));
}

TEST_CASE("TranslateJqlToGitHubSearch — reporter != currentUser() warns on operator") {
    const auto r = TranslateJqlToGitHubSearch("reporter != currentUser()", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Unsupported operator"));
    CHECK(Contains(r.Warning, "reporter"));
}

TEST_CASE("TranslateJqlToGitHubSearch — author field aliases reporter → author:@me") {
    const auto r = TranslateJqlToGitHubSearch("author = currentUser()", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "author:@me");
}

TEST_CASE("TranslateJqlToGitHubSearch — empty label value dropped with warning") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Empty label"));
}

TEST_CASE("TranslateJqlToGitHubSearch — unsupported operator on labels warns") {
    const auto r = TranslateJqlToGitHubSearch("labels ~ \"bug\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Unsupported operator"));
    CHECK(Contains(r.Warning, "labels"));
}

TEST_CASE("TranslateJqlToGitHubSearch — summary ~ phrase aliases text") {
    const auto r = TranslateJqlToGitHubSearch("summary ~ \"crash on exit\"", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "\"crash on exit\"");
}

TEST_CASE("TranslateJqlToGitHubSearch — description = bareword unquoted") {
    const auto r = TranslateJqlToGitHubSearch("description = leak", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "leak");
}

TEST_CASE("TranslateJqlToGitHubSearch — missing value after operator warns") {
    const auto r = TranslateJqlToGitHubSearch("assignee =", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Missing value"));
}

TEST_CASE("TranslateJqlToGitHubSearch — colon shorthand normalizes to = for status") {
    const auto r = TranslateJqlToGitHubSearch("status:open", "", "");
    CHECK(r.Ok);
    CHECK(r.Query == "is:open");
}

TEST_CASE("TranslateJqlToGitHubSearch — unterminated quote → Ok=false with error") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"bug", "", "");
    CHECK_FALSE(r.Ok);
    CHECK_FALSE(r.Error.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — stray bareword dropped with warning") {
    const auto r = TranslateJqlToGitHubSearch("frobnicate", "", "");
    CHECK(r.Ok);
    CHECK(r.Query.empty());
    CHECK(Contains(r.Warning, "Unrecognised token"));
}

TEST_CASE("TranslateJqlToGitHubSearch — ORDER BY after multiple clauses strips tail only") {
    const auto r = TranslateJqlToGitHubSearch("labels = \"bug\" AND status = \"Open\" ORDER BY priority DESC", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "label:bug"));
    CHECK(Contains(r.Query, "is:open"));
    CHECK(Contains(r.Warning, "ORDER BY"));
}

// user-info-window.md Slice 4 item 18c — `key = "owner/repo#N"` carries the canonical
// key out-of-band in KeyFilter (GitHub search has no exact issue-key qualifier).

TEST_CASE("TranslateJqlToGitHubSearch — key clause populates KeyFilter, no query term") {
    const auto r = TranslateJqlToGitHubSearch("key = \"alexandrosk0/Smatchet#42\"", "", "");
    CHECK(r.Ok);
    CHECK(r.KeyFilter == "alexandrosk0/Smatchet#42");
    CHECK(r.Query.empty());
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToGitHubSearch — issuekey alias also populates KeyFilter") {
    const auto r = TranslateJqlToGitHubSearch("issuekey = \"o/r#7\"", "", "");
    CHECK(r.Ok);
    CHECK(r.KeyFilter == "o/r#7");
}

TEST_CASE("TranslateJqlToGitHubSearch — non-canonical key value drops with warning") {
    const auto r = TranslateJqlToGitHubSearch("key = \"SMT-7\"", "", "");
    CHECK(r.Ok);
    CHECK(r.KeyFilter.empty());
    CHECK(Contains(r.Warning, "not a canonical"));
}

TEST_CASE("TranslateJqlToGitHubSearch — unsupported operator on key drops with warning") {
    const auto r = TranslateJqlToGitHubSearch("key != \"o/r#7\"", "", "");
    CHECK(r.Ok);
    CHECK(r.KeyFilter.empty());
    CHECK(Contains(r.Warning, "Unsupported operator"));
}

TEST_CASE("TranslateJqlToGitHubSearch — key clause composes with other clauses") {
    const auto r = TranslateJqlToGitHubSearch("key = \"o/r#7\" AND status = \"Open\"", "", "");
    CHECK(r.Ok);
    CHECK(r.KeyFilter == "o/r#7");
    CHECK(r.Query == "is:open");
}
