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

// Offline-first harness (Quality Pillar 6): network switch, catalog, transitions and comments keys.
// Every case restores the process-wide switch with ScopedFakeNetworkReset so no outage leaks.

namespace {
const char* const kEmptyFetch =
    R"("fetches": [{"fullSyncCompleted": true, "selectedFields": [], "jiraSearchPages": [{"issues": [], "isLast": true}]}])";
} // namespace

TEST_CASE("JiraFakeTrackerFixture::Offline — TransportDown fails network calls like a real outage") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"network": {"mode": "TransportDown"},
      "transitions": {"OFF-1": [{"id": "2", "name": "In Progress"}]}, )") +
                             kEmptyFetch + "}";
    const auto client = JiraFakeTrackerFixture::LoadFromString(json).CreateClient();

    TrackerConfig cfg;
    CHECK(client->ProbeReachability(cfg).Kind == TrackerReachabilityProbeKind::TransportDown);
    const auto transitions = client->FetchIssueTransitions(cfg, "OFF-1");
    REQUIRE_FALSE(static_cast<bool>(transitions));
    CHECK(transitions.error().Kind == TrackerErrorKind::Transport);
    CHECK(transitions.error().IsRetryable());
    // The probe is expected while offline and is not counted; the transitions call is.
    CHECK(smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 1);
    CHECK(client->FetchIssueTransitionsCalls() == 0); // a down call is never recorded
}

TEST_CASE("JiraFakeTrackerFixture::Offline — ServiceUnavailable reports a retryable 503") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"network": {"mode": "ServiceUnavailable"}, )") + kEmptyFetch + "}";
    const auto client = JiraFakeTrackerFixture::LoadFromString(json).CreateClient();

    TrackerConfig cfg;
    CHECK(client->ProbeReachability(cfg).Kind == TrackerReachabilityProbeKind::ServiceUnavailable);
    const TrackerError err = client->UpdateIssueFields("OFF-1", nlohmann::json::object());
    CHECK(err.Kind == TrackerErrorKind::ServerError);
    CHECK(err.IsRetryable());
    CHECK(client->UpdateIssueFieldsCallCount() == 0);
}

TEST_CASE("JiraFakeTrackerFixture::Offline — an unknown network mode is rejected") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"network": {"mode": "Sideways"}, )") + kEmptyFetch + "}";
    CHECK_THROWS(JiraFakeTrackerFixture::LoadFromString(json));
}

TEST_CASE("JiraFakeTrackerFixture::Offline — transitions round-trip per issue") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"transitions": {"OFF-1": [
        {"id": "2", "name": "In Progress"}, {"id": "3", "name": "Done"}]}, )") +
                             kEmptyFetch + "}";
    const auto client = JiraFakeTrackerFixture::LoadFromString(json).CreateClient();

    TrackerConfig cfg;
    const auto result = client->FetchIssueTransitions(cfg, "OFF-1");
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.value().size() == 2);
    CHECK(result.value()[0].Id == "2");
    CHECK(result.value()[0].Value == "In Progress");
    CHECK(result.value()[1].Value == "Done");
    CHECK_FALSE(static_cast<bool>(client->FetchIssueTransitions(cfg, "OFF-2"))); // unscripted
}

TEST_CASE("JiraFakeTrackerFixture::Offline — catalog.fields builds a Status field with its options") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"catalog": {"fields": [
        {"id": "status", "name": "Status", "family": "Status", "options": [
          {"id": "1", "value": "To Do"}, {"id": "2", "value": "In Progress"},
          {"id": "3", "value": "Done"}, {"id": "4", "value": "Blocked"}]},
        {"id": "summary", "name": "Summary", "family": "Text", "options": []}]}, )") +
                             kEmptyFetch + "}";
    const auto client = JiraFakeTrackerFixture::LoadFromString(json).CreateClient();

    TrackerConfig cfg;
    const auto catalog = client->FetchFieldCatalog(cfg, std::string());
    REQUIRE(static_cast<bool>(catalog));
    const auto& fields = catalog.value().Fields;
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].Id == "status");
    CHECK(fields[0].Family == TrackerFieldFamily::Status);
    REQUIRE(fields[0].AllowedValueOptions.size() == 4);
    CHECK(fields[0].AllowedValueOptions[3].Value == "Blocked");
    CHECK(fields[0].AllowedValues.size() == 4);
    CHECK(fields[1].Family == TrackerFieldFamily::Text);
}

TEST_CASE("JiraFakeTrackerFixture::Offline — an empty catalog keeps the not-supported default") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const auto client = JiraFakeTrackerFixture::LoadFromString(kBasicFixture).CreateClient();
    TrackerConfig cfg;
    const auto catalog = client->FetchFieldCatalog(cfg, std::string());
    REQUIRE_FALSE(static_cast<bool>(catalog));
    CHECK(catalog.error().Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraFakeTrackerFixture::Offline — comments enable Collaboration and round-trip") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const std::string json = std::string(R"({"comments": {"OFF-1": [
        {"id": "c1", "author": "Ana Offline", "body": "cached comment body", "createdAtSec": 1700000000}]}, )") +
                             kEmptyFetch + "}";
    const auto client = JiraFakeTrackerFixture::LoadFromString(json).CreateClient();

    ITrackerCollaboration* collab = client->Collaboration();
    REQUIRE(collab != nullptr);
    const auto comments = collab->FetchIssueComments("OFF-1");
    REQUIRE(static_cast<bool>(comments));
    REQUIRE(comments.value().size() == 1);
    CHECK(comments.value()[0].Author == "Ana Offline");
    CHECK(comments.value()[0].Body == "cached comment body");
    CHECK(comments.value()[0].CreatedAtSec == 1700000000);
    CHECK(comments.value()[0].UpdatedAtSec == 1700000000);
}

TEST_CASE("JiraFakeTrackerFixture::Offline — no network key leaves the switch up") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    const auto client = JiraFakeTrackerFixture::LoadFromString(kBasicFixture).CreateClient();
    CHECK_FALSE(smatchet_tests::GlobalFakeNetwork().IsDown());
    TrackerConfig cfg;
    CHECK(client->ProbeReachability(cfg).Kind == TrackerReachabilityProbeKind::AuthenticatedReachable);
    CHECK(client->Collaboration() == nullptr); // no comments scripted
}

TEST_CASE("JiraFakeTrackerFixture::Offline — a new client never resets an outage already in force") {
    smatchet_tests::ScopedFakeNetworkReset reset;
    smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
    const auto client = JiraFakeTrackerFixture::LoadFromString(kBasicFixture).CreateClient();
    CHECK(smatchet_tests::GlobalFakeNetwork().IsDown());
    TrackerConfig cfg;
    CHECK(client->ProbeReachability(cfg).Kind == TrackerReachabilityProbeKind::TransportDown);
}
