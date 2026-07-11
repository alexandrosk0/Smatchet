// JqlQuoteAware doctest — DR24 regression coverage for the pure JQL translators.
// Covers: Linear tokenizers made quote-aware (connector/operator substrings
// inside quoted values are atomic; the text search survives), the GitHub
// tokenizer preserving `@` (so `@me` is not collapsed to `me`), MaybeQuote
// escaping embedded quotes, and the structural `owner/repo` project anchor
// (a stray `/` inside a value such as a `ui/ux` label is not a repo anchor).

#include "GitHubQueryFromJql.h"
#include "LinearQueryFromJql.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Linear — quote-aware AND splitting / operator detection / OR rejection.
// ---------------------------------------------------------------------------

TEST_CASE("Linear DR24 — ` and ` inside a quoted text value does not split the clause") {
    using namespace smatchet::linear;
    // Before the fix, SplitTopLevelAnd matched the ` and ` inside the quotes and
    // split mid-value, corrupting both halves and dropping the text search.
    JqlToLinearResult r = TranslateJqlToLinearFilter("text ~ \"cut and paste\"");
    CHECK(r.Ok);
    CHECK(r.SearchTerm == "cut and paste");
}

TEST_CASE("Linear DR24 — top-level AND still splits around a quoted value") {
    using namespace smatchet::linear;
    JqlToLinearResult r = TranslateJqlToLinearFilter("text ~ \"cut and paste\" AND status = Done");
    CHECK(r.Ok);
    CHECK(r.SearchTerm == "cut and paste");
    CHECK(r.Filter["state"]["name"]["eq"] == "Done");
}

TEST_CASE("Linear DR24 — ` in ` inside a quoted value is not treated as the IN operator") {
    using namespace smatchet::linear;
    // `summary ~ "work in progress"` must detect `~` (contains), not the ` in `
    // buried in the value. Before the fix the ` in ` won and the clause was junk.
    JqlToLinearResult r = TranslateJqlToLinearFilter("summary ~ \"work in progress\"");
    CHECK(r.Ok);
    CHECK(r.SearchTerm == "work in progress");
}

TEST_CASE("Linear DR24 — `=` inside a quoted value is not treated as the eq operator") {
    using namespace smatchet::linear;
    JqlToLinearResult r = TranslateJqlToLinearFilter("description ~ \"a = b\"");
    CHECK(r.Ok);
    CHECK(r.SearchTerm == "a = b");
}

TEST_CASE("Linear DR24 — ` or ` inside a quoted text value does not trigger OR rejection") {
    using namespace smatchet::linear;
    JqlToLinearResult r = TranslateJqlToLinearFilter("summary ~ \"cats or dogs\"");
    CHECK(r.Ok);
    CHECK(r.SearchTerm == "cats or dogs");
    CHECK_FALSE(Contains(r.Warning, "OR is not supported"));
}

TEST_CASE("Linear DR24 — a genuine top-level OR is still rejected") {
    using namespace smatchet::linear;
    JqlToLinearResult r = TranslateJqlToLinearFilter("status = Done or status = Todo");
    CHECK(Contains(r.Warning, "OR is not supported"));
}

TEST_CASE("Linear DR24 — real IN list still parses (quote-awareness regression guard)") {
    using namespace smatchet::linear;
    JqlToLinearResult r = TranslateJqlToLinearFilter("status in (\"Todo\", \"In Progress\")");
    REQUIRE(r.Filter["state"]["name"]["in"].is_array());
    CHECK(r.Filter["state"]["name"]["in"].size() == 2);
    CHECK(r.Filter["state"]["name"]["in"][1] == "In Progress");
}

// ---------------------------------------------------------------------------
// GitHub — tokenizer preserves `@`; MaybeQuote escapes embedded quotes.
// ---------------------------------------------------------------------------

TEST_CASE("GitHub DR24 — `@me` survives tokenization intact (not collapsed to `me`)") {
    using namespace smatchet::github;
    JqlToGitHubResult r = TranslateJqlToGitHubSearch("assignee = @me", "", "");
    CHECK(r.Ok);
    CHECK(Contains(r.Query, "assignee:@me"));
    CHECK_FALSE(Contains(r.Query, "assignee:me"));
}

TEST_CASE("GitHub DR24 — currentUser() still maps to @me (regression guard)") {
    using namespace smatchet::github;
    JqlToGitHubResult r = TranslateJqlToGitHubSearch("assignee = currentUser()", "", "");
    CHECK(Contains(r.Query, "assignee:@me"));
}

TEST_CASE("GitHub DR24 — MaybeQuote escapes an embedded quote in a whitespace value") {
    using namespace smatchet::github;
    // A single-quoted JQL value can carry literal double-quotes (the tokenizer
    // only strips the outer single quotes). When that value has whitespace it is
    // wrapped in double quotes, and the inner `"` MUST be backslash-escaped so it
    // cannot terminate the qualifier early. JQL: labels = 'needs "review" now'
    JqlToGitHubResult r = TranslateJqlToGitHubSearch("labels = 'needs \"review\" now'", "", "");
    CHECK(r.Ok);
    // Emitted qualifier keeps the inner quotes escaped: label:"needs \"review\" now"
    CHECK(Contains(r.Query, "label:\"needs \\\"review\\\" now\""));
}

TEST_CASE("GitHub DR24 — plain whitespace label is still wrapped without stray escapes") {
    using namespace smatchet::github;
    JqlToGitHubResult r = TranslateJqlToGitHubSearch("labels = \"needs review\"", "", "");
    CHECK(Contains(r.Query, "label:\"needs review\""));
    CHECK_FALSE(Contains(r.Query, "\\"));
}

// ---------------------------------------------------------------------------
// GitHub — structural owner/repo project anchor.
// ---------------------------------------------------------------------------

TEST_CASE("GitHub DR24 — ExtractGitHubProjectAnchor accepts a leading owner/repo token") {
    using namespace smatchet::github;
    CHECK(ExtractGitHubProjectAnchor("octocat/hello-world") == "octocat/hello-world");
    CHECK(ExtractGitHubProjectAnchor("octocat/hello-world is:open") == "octocat/hello-world");
}

TEST_CASE("GitHub DR24 — a `ui/ux` label value is NOT mistaken for a project anchor") {
    using namespace smatchet::github;
    // The bug: any query containing `/` returned the first token as the project.
    CHECK(ExtractGitHubProjectAnchor("label:ui/ux").empty());
    CHECK(ExtractGitHubProjectAnchor("is:open label:ui/ux").empty());
    CHECK(ExtractGitHubProjectAnchor("type:issue ui/ux").empty());
}

TEST_CASE("GitHub DR24 — tokens without a valid owner/repo shape yield empty anchor") {
    using namespace smatchet::github;
    CHECK(ExtractGitHubProjectAnchor("").empty());
    CHECK(ExtractGitHubProjectAnchor("is:open").empty());
    CHECK(ExtractGitHubProjectAnchor("/repo").empty());
    CHECK(ExtractGitHubProjectAnchor("owner/").empty());
    CHECK(ExtractGitHubProjectAnchor("a/b/c").empty());
}
