#include <doctest/doctest.h>

#include "Ui/GridSearchInputClassifier.h"

#include <string>

using smatchet::gridsearch::AppliesAsRowFilter;
using smatchet::gridsearch::ClassifyGridSearchInput;
using smatchet::gridsearch::GridSearchBackend;
using smatchet::gridsearch::GridSearchBackendFromKey;
using smatchet::gridsearch::GridSearchInputKind;
using smatchet::gridsearch::LooksLikeStructuredQuery;
using smatchet::gridsearch::LooksLikeTicketKey;

TEST_CASE("GridSearchBackendFromKey: case-insensitive map, unknown defaults to Jira") {
    CHECK(GridSearchBackendFromKey("Jira") == GridSearchBackend::Jira);
    CHECK(GridSearchBackendFromKey("jira") == GridSearchBackend::Jira);
    CHECK(GridSearchBackendFromKey("Plane") == GridSearchBackend::Plane);
    CHECK(GridSearchBackendFromKey("PLANE") == GridSearchBackend::Plane);
    CHECK(GridSearchBackendFromKey("GitHub") == GridSearchBackend::GitHub);
    CHECK(GridSearchBackendFromKey("github") == GridSearchBackend::GitHub);
    // Unknown / empty / stale → Jira (matches DefaultTrackerBackendFactory fallback).
    CHECK(GridSearchBackendFromKey("") == GridSearchBackend::Jira);
    CHECK(GridSearchBackendFromKey("Gitlab") == GridSearchBackend::Jira);
}

TEST_CASE("LooksLikeTicketKey: Jira whole-string key shape") {
    CHECK(LooksLikeTicketKey("PROJ-123", GridSearchBackend::Jira));
    CHECK(LooksLikeTicketKey("ABC-1", GridSearchBackend::Jira));
    // Lowercase prefix still has the key shape (ExtractIssueKeyPrefix is case-tolerant).
    CHECK(LooksLikeTicketKey("proj-7", GridSearchBackend::Jira));
    // Not a bare key:
    CHECK_FALSE(LooksLikeTicketKey("PROJ", GridSearchBackend::Jira));         // no number
    CHECK_FALSE(LooksLikeTicketKey("123", GridSearchBackend::Jira));          // digits only
    CHECK_FALSE(LooksLikeTicketKey("-123", GridSearchBackend::Jira));         // leading dash
    CHECK_FALSE(LooksLikeTicketKey("login button", GridSearchBackend::Jira)); // plain words
    // A UUID must NOT be misread as a key (digit-leading is rejected).
    CHECK_FALSE(LooksLikeTicketKey("550e8400-e29b-41d4-a716-446655440000", GridSearchBackend::Jira));
    // Multi-token query is not a bare key (the whole-string validator rejects spaces).
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123 AND status = Open", GridSearchBackend::Jira));
}

TEST_CASE("LooksLikeTicketKey: GitHub owner/repo#N key shape") {
    CHECK(LooksLikeTicketKey("alexandrosk0/Smatchet#42", GridSearchBackend::GitHub));
    CHECK(LooksLikeTicketKey("owner/repo#1", GridSearchBackend::GitHub));
    // A Jira-shaped key is NOT a GitHub key.
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123", GridSearchBackend::GitHub));
    CHECK_FALSE(LooksLikeTicketKey("owner/repo", GridSearchBackend::GitHub)); // no #N
    CHECK_FALSE(LooksLikeTicketKey("repo#42", GridSearchBackend::GitHub));    // no owner/
}

TEST_CASE("LooksLikeTicketKey: Plane never yields a ticket key") {
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123", GridSearchBackend::Plane));
    CHECK_FALSE(LooksLikeTicketKey("owner/repo#42", GridSearchBackend::Plane));
    CHECK_FALSE(LooksLikeTicketKey("550e8400-e29b-41d4-a716-446655440000", GridSearchBackend::Plane));
}

TEST_CASE("LooksLikeStructuredQuery: comparison / grouping chars mark a filter") {
    CHECK(LooksLikeStructuredQuery("status = Open"));
    CHECK(LooksLikeStructuredQuery("summary ~ login"));
    CHECK(LooksLikeStructuredQuery("priority != Low"));
    CHECK(LooksLikeStructuredQuery("created >= -7d"));
    CHECK(LooksLikeStructuredQuery("labels in (a, b)"));
}

TEST_CASE("LooksLikeStructuredQuery: field:value colon token") {
    CHECK(LooksLikeStructuredQuery("is:open"));
    CHECK(LooksLikeStructuredQuery("state:done label:bug"));
    // Free-text colon (space after) is NOT a filter token.
    CHECK_FALSE(LooksLikeStructuredQuery("fix: crash on startup"));
}

TEST_CASE("LooksLikeStructuredQuery: JQL empty/null predicates (uppercase keywords)") {
    // These carry no operator char of their own — without an explicit rule they classified as
    // TitleSearch, so Enter was a no-op and the query stayed an active row filter that hid
    // every loaded row.
    CHECK(LooksLikeStructuredQuery("project IS EMPTY"));
    CHECK(LooksLikeStructuredQuery("assignee IS NOT EMPTY"));
    CHECK(LooksLikeStructuredQuery("fixVersion IS NULL"));
    CHECK(LooksLikeStructuredQuery("resolution IS NOT NULL"));
    // Whole-word span only: "TH|IS EMPTY" must not match on the substring split.
    CHECK_FALSE(LooksLikeStructuredQuery("THIS EMPTY grid"));
    CHECK_FALSE(LooksLikeStructuredQuery("WHATIS NULLABLE"));
    // Uppercase-only by design: lowercase "is empty" is an ordinary ticket title, and no
    // heuristic separates it from a lowercase query without knowing the field names.
    CHECK_FALSE(LooksLikeStructuredQuery("cart is empty after reload"));
    CHECK_FALSE(LooksLikeStructuredQuery("project is empty"));
}

TEST_CASE("ClassifyGridSearchInput: an empty/null predicate is a query, not a row filter") {
    CHECK(ClassifyGridSearchInput("project IS EMPTY", GridSearchBackend::Jira) == GridSearchInputKind::Jql);
    CHECK_FALSE(AppliesAsRowFilter(ClassifyGridSearchInput("project IS EMPTY", GridSearchBackend::Jira)));
    // The prose counterpart still filters the loaded rows as typed.
    CHECK(AppliesAsRowFilter(ClassifyGridSearchInput("cart is empty after reload", GridSearchBackend::Jira)));
}

TEST_CASE("LooksLikeStructuredQuery: order-by clause") {
    CHECK(LooksLikeStructuredQuery("ORDER BY created DESC"));
    CHECK(LooksLikeStructuredQuery("order by updated"));
}

TEST_CASE("LooksLikeStructuredQuery: plain title is not structured") {
    CHECK_FALSE(LooksLikeStructuredQuery("login button broken"));
    CHECK_FALSE(LooksLikeStructuredQuery("drag and drop reorder"));
    CHECK_FALSE(LooksLikeStructuredQuery("crash on startup"));
}

TEST_CASE("ClassifyGridSearchInput: empty / whitespace degrades to TitleSearch") {
    CHECK(ClassifyGridSearchInput("", GridSearchBackend::Jira) == GridSearchInputKind::TitleSearch);
    CHECK(ClassifyGridSearchInput("   \t ", GridSearchBackend::Jira) == GridSearchInputKind::TitleSearch);
}

TEST_CASE("ClassifyGridSearchInput: precedence ticket-key > jql > title (Jira)") {
    CHECK(ClassifyGridSearchInput("PROJ-123", GridSearchBackend::Jira) == GridSearchInputKind::TicketKey);
    // Leading / trailing whitespace is trimmed before classifying.
    CHECK(ClassifyGridSearchInput("  PROJ-123  ", GridSearchBackend::Jira) == GridSearchInputKind::TicketKey);
    CHECK(ClassifyGridSearchInput("status = Open", GridSearchBackend::Jira) == GridSearchInputKind::Jql);
    CHECK(ClassifyGridSearchInput("login button broken", GridSearchBackend::Jira) == GridSearchInputKind::TitleSearch);
    // A key embedded in a larger clause classifies as Jql, not TicketKey.
    CHECK(ClassifyGridSearchInput("PROJ-123 AND status = Open", GridSearchBackend::Jira) == GridSearchInputKind::Jql);
}

TEST_CASE("ClassifyGridSearchInput: GitHub key routes to TicketKey") {
    CHECK(ClassifyGridSearchInput("owner/repo#42", GridSearchBackend::GitHub) == GridSearchInputKind::TicketKey);
    // GitHub search query uses key:value → Jql.
    CHECK(ClassifyGridSearchInput("is:open label:bug", GridSearchBackend::GitHub) == GridSearchInputKind::Jql);
    // Plain words → TitleSearch.
    CHECK(ClassifyGridSearchInput("dark mode", GridSearchBackend::GitHub) == GridSearchInputKind::TitleSearch);
}

TEST_CASE("ClassifyGridSearchInput: Plane never produces TicketKey") {
    // A Jira-shaped string on a Plane pane is a title search, not a jump.
    CHECK(ClassifyGridSearchInput("PROJ-123", GridSearchBackend::Plane) == GridSearchInputKind::TitleSearch);
    CHECK(ClassifyGridSearchInput("state:done", GridSearchBackend::Plane) == GridSearchInputKind::Jql);
    CHECK(ClassifyGridSearchInput("payment page", GridSearchBackend::Plane) == GridSearchInputKind::TitleSearch);
}

TEST_CASE("AppliesAsRowFilter: everything but a structured query filters the loaded rows") {
    // Plain words and a ticket key are row content — matching them narrows the grid as the
    // user types. A structured query is not: it addresses the backend, and it stays in the box
    // after its own Enter re-runs the view, so matching it would hide every row just fetched.
    CHECK(AppliesAsRowFilter(GridSearchInputKind::TitleSearch));
    CHECK(AppliesAsRowFilter(GridSearchInputKind::TicketKey));
    CHECK_FALSE(AppliesAsRowFilter(GridSearchInputKind::Jql));

    // End-to-end through the classifier.
    CHECK_FALSE(AppliesAsRowFilter(ClassifyGridSearchInput("status = Open", GridSearchBackend::Jira)));
    CHECK(AppliesAsRowFilter(ClassifyGridSearchInput("login button", GridSearchBackend::Jira)));
    CHECK(AppliesAsRowFilter(ClassifyGridSearchInput("PROJ-123", GridSearchBackend::Jira)));
    // An empty box classifies as TitleSearch, and an empty filter matches every row.
    CHECK(AppliesAsRowFilter(ClassifyGridSearchInput("", GridSearchBackend::Jira)));
}
