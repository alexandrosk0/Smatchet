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
