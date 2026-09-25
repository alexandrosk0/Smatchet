// Slice 2 of deterministic-jira-test-backend — Bucket A tests
// for JiraFakeTrackerFixture (fixture JSON parser + FakeTrackerClient configurator).
// No HTTP, no AppController, no threading.

#include "JiraFakeTrackerFixture.h"
#include "FakeTrackerClient.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet_tests::FakeTrackerClient;
using smatchet_tests::JiraFakeTrackerFixture;

namespace {

const char* kBasicFixture = R"({
  "name": "basic-grid",
  "reachability": {"kind": "AuthenticatedReachable", "diagnostic": ""},
  "fetches": [
    {
      "fullSyncCompleted": true,
      "warning": "",
      "fetchError": "",
      "selectedFields": ["summary", "status"],
      "jiraSearchPages": [
        {
          "issues": [
            {"key": "SMAT-1", "fields": {"summary": "First issue", "status": {"name": "Open"}}},
            {"key": "SMAT-2", "fields": {"summary": "Second issue", "status": {"name": "Done"}}}
          ],
          "isLast": true
        }
      ]
    }
  ],
  "mutations": {
    "updateIssueFields": [{"ok": true}],
    "createIssue": [{"ok": true, "issueKey": "SMAT-99"}]
  }
})";

const char* kTransportErrorFixture = R"({
  "name": "transport-error-after-cache",
  "reachability": {"kind": "AuthenticatedReachable", "diagnostic": ""},
  "fetches": [
    {
      "fullSyncCompleted": true,
      "warning": "",
      "fetchError": "",
      "selectedFields": ["summary"],
      "jiraSearchPages": [
        {"issues": [{"key": "SMAT-1", "fields": {"summary": "Cached"}}], "isLast": true}
      ]
    },
    {
      "fullSyncCompleted": false,
      "warning": "",
      "fetchError": "HTTP 503",
      "selectedFields": [],
      "jiraSearchPages": []
    }
  ]
})";

const char* kCachedTicketsFixture = R"({
  "name": "cached-tickets-path",
  "reachability": {"kind": "TransportDown", "diagnostic": "offline"},
  "fetches": [
    {
      "fullSyncCompleted": false,
      "fetchError": "",
      "cachedTickets": [
        {"id": "PLAIN-1", "fields": {"summary": "Plain ticket", "status": "Backlog"}}
      ]
    }
  ]
})";

} // namespace

TEST_CASE("JiraFakeTrackerFixture::LoadFromString — basic fixture parses and configures client") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    const auto client = fixture.CreateClient();

    CHECK(client->GetTrackerType() == "Jira");

    bool fullSync = false;
    std::string fetchError;
    const auto tickets = client->FetchIssues(&fullSync, nullptr, nullptr, &fetchError);

    CHECK(fullSync);
    CHECK(fetchError.empty());
    REQUIRE(tickets.size() == 2);
    CHECK(tickets[0].id == "SMAT-1");
    CHECK(tickets[1].id == "SMAT-2");
}

TEST_CASE("JiraFakeTrackerFixture — jiraSearchPages flow through AppendCachedTicketFromJiraSearchIssue") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    const auto client = fixture.CreateClient();

    const auto tickets = client->FetchIssues();

    REQUIRE(tickets.size() == 2);
    const auto it = tickets[0].fieldValues.find("summary");
    REQUIRE(it != tickets[0].fieldValues.end());
    CHECK(it->second == "First issue");
}

TEST_CASE("JiraFakeTrackerFixture — second fetch returns scripted transport error") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kTransportErrorFixture);
    const auto client = fixture.CreateClient();

    bool fullSync1 = false;
    std::string err1;
    const auto tickets1 = client->FetchIssues(&fullSync1, nullptr, nullptr, &err1);
    CHECK(fullSync1);
    CHECK(err1.empty());
    CHECK(tickets1.size() == 1);

    bool fullSync2 = false;
    std::string err2;
    const auto tickets2 = client->FetchIssues(&fullSync2, nullptr, nullptr, &err2);
    CHECK_FALSE(fullSync2);
    CHECK(err2 == "HTTP 503");
    CHECK(tickets2.empty());
}

TEST_CASE("JiraFakeTrackerFixture — cachedTickets path populates tickets without Jira JSON parsing") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kCachedTicketsFixture);
    const auto client = fixture.CreateClient();

    bool fullSync = true;
    const auto tickets = client->FetchIssues(&fullSync);

    CHECK_FALSE(fullSync);
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "PLAIN-1");
    const auto it = tickets[0].fieldValues.find("summary");
    REQUIRE(it != tickets[0].fieldValues.end());
    CHECK(it->second == "Plain ticket");
}

TEST_CASE("JiraFakeTrackerFixture — reachability probe returns scripted kind") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kCachedTicketsFixture);
    const auto client = fixture.CreateClient();

    TrackerConfig cfg;
    const auto result = client->ProbeReachability(cfg);
    CHECK(result.Kind == TrackerReachabilityProbeKind::TransportDown);
    CHECK(result.Diagnostic == "offline");
}

TEST_CASE("JiraFakeTrackerFixture — UpdateIssueFields scripted success") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    const auto client = fixture.CreateClient();

    const TrackerError err = client->UpdateIssueFields("SMAT-1", {{"summary", "new"}});
    CHECK(err.IsOk());
    CHECK(err.Detail.empty());
    CHECK(client->UpdateIssueFieldsCallCount() == 1);
}

TEST_CASE("JiraFakeTrackerFixture — CreateIssue scripted success returns key") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    const auto client = fixture.CreateClient();

    const auto result = client->CreateIssue({{"summary", "new"}});
    REQUIRE(static_cast<bool>(result));
    CHECK(result.value() == "SMAT-99");
}

TEST_CASE("JiraFakeTrackerFixture — CreateClient produces independent instances") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);

    const auto clientA = fixture.CreateClient();
    const auto clientB = fixture.CreateClient();

    // Drain clientA's fetch queue
    clientA->FetchIssues();
    // clientB should still have its own queue intact
    bool fullSync = false;
    const auto tickets = clientB->FetchIssues(&fullSync);
    CHECK(fullSync);
    CHECK(tickets.size() == 2);
}

TEST_CASE("JiraFakeTrackerFixture::LoadFromString — malformed JSON throws") {
    CHECK_THROWS_AS(JiraFakeTrackerFixture::LoadFromString("not json {{{"), std::runtime_error);
}

TEST_CASE("JiraFakeTrackerFixture — Configure applies to pre-existing client") {
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    FakeTrackerClient client("Jira");
    fixture.Configure(client);

    const auto tickets = client.FetchIssues();
    CHECK(tickets.size() == 2);
}

// Offline-first (Slice 3) tests for network mode, catalog, transitions, comments parsing

TEST_CASE("JiraFakeTrackerFixture::Offline — network mode TransportDown parsed and applied") {
    ScopedFakeNetworkReset netReset;
    const char* offlineFixture = R"({
      "network": {"mode": "TransportDown"},
      "fetches": [
        {"fullSyncCompleted": true, "selectedFields": [], "jiraSearchPages": [{"issues": [], "isLast": true}]}
      ]
    })";

    const auto fixture = JiraFakeTrackerFixture::LoadFromString(offlineFixture);
    const auto client = fixture.CreateClient();

    // ProbeReachability with network down should return TransportDown
    TrackerConfig cfg;
    const auto result = client->ProbeReachability(cfg);
    CHECK(result.Kind == TrackerReachabilityProbeKind::TransportDown);
}

TEST_CASE("JiraFakeTrackerFixture::Offline — field catalog parsed from catalog.fields") {
    const char* catalogFixture = R"({
      "catalog": {
        "fields": [
          {"id": "status", "name": "Status", "family": "Status", "options": [
            {"id": "1", "value": "To Do"},
            {"id": "2", "value": "Done"}
          ]},
          {"id": "summary", "name": "Summary", "family": "Text", "options": []}
        ]
      },
      "fetches": [
        {"fullSyncCompleted": true, "selectedFields": [], "jiraSearchPages": [{"issues": [], "isLast": true}]}
      ]
    })";

    const auto fixture = JiraFakeTrackerFixture::LoadFromString(catalogFixture);
    const auto client = fixture.CreateClient();

    // FetchFieldCatalog should return the scripted fields
    TrackerConfig cfg;
    const auto catalogResult = client->FetchFieldCatalog(cfg, "TEST");
    REQUIRE(static_cast<bool>(catalogResult));
    const auto& fields = catalogResult.value().Fields;
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].Id == "status");
    CHECK(fields[0].AllowedValueOptions.size() == 2);
    CHECK(fields[1].Id == "summary");
}

TEST_CASE("JiraFakeTrackerFixture::Offline — issue transitions parsed per key") {
    const char* transitionsFixture = R"({
      "transitions": {
        "OFF-1": [
          {"id": "2", "name": "In Progress"},
          {"id": "3", "name": "Done"}
        ]
      },
      "fetches": [
        {"fullSyncCompleted": true, "selectedFields": [], "jiraSearchPages": [{"issues": [], "isLast": true}]}
      ]
    })";

    const auto fixture = JiraFakeTrackerFixture::LoadFromString(transitionsFixture);
    const auto client = fixture.CreateClient();

    // Collaboration must be enabled for transitions to be accessible
    CHECK(client->Collaboration() != nullptr);

    TrackerConfig cfg;
    const auto result = client->FetchIssueTransitions(cfg, "OFF-1");
    REQUIRE(static_cast<bool>(result));
    const auto& transitions = result.value();
    REQUIRE(transitions.size() == 2);
    CHECK(transitions[0].Value == "In Progress");
    CHECK(transitions[1].Value == "Done");
}

TEST_CASE("JiraFakeTrackerFixture::Offline — issue comments parsed per key") {
    const char* commentsFixture = R"({
      "comments": {
        "OFF-1": [
          {"id": "c1", "author": "Alice", "body": "First comment", "createdAtSec": 1000},
          {"id": "c2", "author": "Bob", "body": "Second comment", "createdAtSec": 2000}
        ]
      },
      "fetches": [
        {"fullSyncCompleted": true, "selectedFields": [], "jiraSearchPages": [{"issues": [], "isLast": true}]}
      ]
    })";

    const auto fixture = JiraFakeTrackerFixture::LoadFromString(commentsFixture);
    const auto client = fixture.CreateClient();

    // Collaboration must be enabled for comments to be accessible
    CHECK(client->Collaboration() != nullptr);

    const auto result = client->FetchIssueComments("OFF-1");
    REQUIRE(static_cast<bool>(result));
    const auto& comments = result.value();
    REQUIRE(comments.size() == 2);
    CHECK(comments[0].Author == "Alice");
    CHECK(comments[0].Body == "First comment");
    CHECK(comments[1].Author == "Bob");
}

TEST_CASE("JiraFakeTrackerFixture::Offline — fixture without network key does not block") {
    // Backward compatibility: fixtures without "network" key should still work
    const auto fixture = JiraFakeTrackerFixture::LoadFromString(kBasicFixture);
    const auto client = fixture.CreateClient();

    // Should not crash; network should be UP by default (no outage)
    TrackerConfig cfg;
    const auto result = client->ProbeReachability(cfg);
    CHECK(result.Kind == TrackerReachabilityProbeKind::AuthenticatedReachable);
}
