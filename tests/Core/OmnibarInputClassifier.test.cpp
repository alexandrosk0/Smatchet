#include <doctest/doctest.h>

#include "Ui/OmnibarInputClassifier.h"

#include <string>

using smatchet::omnibar::ClassifyOmnibarInput;
using smatchet::omnibar::LooksLikeStructuredQuery;
using smatchet::omnibar::LooksLikeTicketKey;
using smatchet::omnibar::OmnibarBackend;
using smatchet::omnibar::OmnibarBackendFromKey;
using smatchet::omnibar::OmnibarInputKind;

TEST_CASE("OmnibarBackendFromKey: case-insensitive map, unknown defaults to Jira") {
    CHECK(OmnibarBackendFromKey("Jira") == OmnibarBackend::Jira);
    CHECK(OmnibarBackendFromKey("jira") == OmnibarBackend::Jira);
    CHECK(OmnibarBackendFromKey("Plane") == OmnibarBackend::Plane);
    CHECK(OmnibarBackendFromKey("PLANE") == OmnibarBackend::Plane);
    CHECK(OmnibarBackendFromKey("GitHub") == OmnibarBackend::GitHub);
    CHECK(OmnibarBackendFromKey("github") == OmnibarBackend::GitHub);
    // Unknown / empty / stale → Jira (matches DefaultTrackerBackendFactory fallback).
    CHECK(OmnibarBackendFromKey("") == OmnibarBackend::Jira);
    CHECK(OmnibarBackendFromKey("Gitlab") == OmnibarBackend::Jira);
}

TEST_CASE("LooksLikeTicketKey: Jira whole-string key shape") {
    CHECK(LooksLikeTicketKey("PROJ-123", OmnibarBackend::Jira));
    CHECK(LooksLikeTicketKey("ABC-1", OmnibarBackend::Jira));
    // Lowercase prefix still has the key shape (ExtractIssueKeyPrefix is case-tolerant).
    CHECK(LooksLikeTicketKey("proj-7", OmnibarBackend::Jira));
    // Not a bare key:
    CHECK_FALSE(LooksLikeTicketKey("PROJ", OmnibarBackend::Jira));         // no number
    CHECK_FALSE(LooksLikeTicketKey("123", OmnibarBackend::Jira));          // digits only
    CHECK_FALSE(LooksLikeTicketKey("-123", OmnibarBackend::Jira));         // leading dash
    CHECK_FALSE(LooksLikeTicketKey("login button", OmnibarBackend::Jira)); // plain words
    // A UUID must NOT be misread as a key (digit-leading is rejected).
    CHECK_FALSE(LooksLikeTicketKey("550e8400-e29b-41d4-a716-446655440000", OmnibarBackend::Jira));
    // Multi-token query is not a bare key (the whole-string validator rejects spaces).
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123 AND status = Open", OmnibarBackend::Jira));
}

TEST_CASE("LooksLikeTicketKey: GitHub owner/repo#N key shape") {
    CHECK(LooksLikeTicketKey("alexandrosk0/Smatchet#42", OmnibarBackend::GitHub));
    CHECK(LooksLikeTicketKey("owner/repo#1", OmnibarBackend::GitHub));
    // A Jira-shaped key is NOT a GitHub key.
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123", OmnibarBackend::GitHub));
    CHECK_FALSE(LooksLikeTicketKey("owner/repo", OmnibarBackend::GitHub)); // no #N
    CHECK_FALSE(LooksLikeTicketKey("repo#42", OmnibarBackend::GitHub));    // no owner/
}

TEST_CASE("LooksLikeTicketKey: Plane never yields a ticket key") {
    CHECK_FALSE(LooksLikeTicketKey("PROJ-123", OmnibarBackend::Plane));
    CHECK_FALSE(LooksLikeTicketKey("owner/repo#42", OmnibarBackend::Plane));
    CHECK_FALSE(LooksLikeTicketKey("550e8400-e29b-41d4-a716-446655440000", OmnibarBackend::Plane));
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

TEST_CASE("LooksLikeStructuredQuery: order-by clause") {
    CHECK(LooksLikeStructuredQuery("ORDER BY created DESC"));
    CHECK(LooksLikeStructuredQuery("order by updated"));
}

TEST_CASE("LooksLikeStructuredQuery: plain title is not structured") {
    CHECK_FALSE(LooksLikeStructuredQuery("login button broken"));
    CHECK_FALSE(LooksLikeStructuredQuery("drag and drop reorder"));
    CHECK_FALSE(LooksLikeStructuredQuery("crash on startup"));
}

TEST_CASE("ClassifyOmnibarInput: empty / whitespace degrades to TitleSearch") {
    CHECK(ClassifyOmnibarInput("", OmnibarBackend::Jira) == OmnibarInputKind::TitleSearch);
    CHECK(ClassifyOmnibarInput("   \t ", OmnibarBackend::Jira) == OmnibarInputKind::TitleSearch);
}

TEST_CASE("ClassifyOmnibarInput: precedence ticket-key > jql > title (Jira)") {
    CHECK(ClassifyOmnibarInput("PROJ-123", OmnibarBackend::Jira) == OmnibarInputKind::TicketKey);
    // Leading / trailing whitespace is trimmed before classifying.
    CHECK(ClassifyOmnibarInput("  PROJ-123  ", OmnibarBackend::Jira) == OmnibarInputKind::TicketKey);
    CHECK(ClassifyOmnibarInput("status = Open", OmnibarBackend::Jira) == OmnibarInputKind::Jql);
    CHECK(ClassifyOmnibarInput("login button broken", OmnibarBackend::Jira) == OmnibarInputKind::TitleSearch);
    // A key embedded in a larger clause classifies as Jql, not TicketKey.
    CHECK(ClassifyOmnibarInput("PROJ-123 AND status = Open", OmnibarBackend::Jira) == OmnibarInputKind::Jql);
}

TEST_CASE("ClassifyOmnibarInput: GitHub key routes to TicketKey") {
    CHECK(ClassifyOmnibarInput("owner/repo#42", OmnibarBackend::GitHub) == OmnibarInputKind::TicketKey);
    // GitHub search query uses key:value → Jql.
    CHECK(ClassifyOmnibarInput("is:open label:bug", OmnibarBackend::GitHub) == OmnibarInputKind::Jql);
    // Plain words → TitleSearch.
    CHECK(ClassifyOmnibarInput("dark mode", OmnibarBackend::GitHub) == OmnibarInputKind::TitleSearch);
}

TEST_CASE("ClassifyOmnibarInput: Plane never produces TicketKey") {
    // A Jira-shaped string on a Plane pane is a title search, not a jump.
    CHECK(ClassifyOmnibarInput("PROJ-123", OmnibarBackend::Plane) == OmnibarInputKind::TitleSearch);
    CHECK(ClassifyOmnibarInput("state:done", OmnibarBackend::Plane) == OmnibarInputKind::Jql);
    CHECK(ClassifyOmnibarInput("payment page", OmnibarBackend::Plane) == OmnibarInputKind::TitleSearch);
}
