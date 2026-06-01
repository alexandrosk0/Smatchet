#ifndef SMATCHET_TESTS_JIRA_FAKE_TRACKER_FIXTURE_H
#define SMATCHET_TESTS_JIRA_FAKE_TRACKER_FIXTURE_H

// Slice 2 of docs/plans/active/deterministic-jira-test-backend.md — Jira fixture
// parser and configurator. Loads a JSON fixture into an immutable script, then
// stamps a fresh FakeTrackerClient("Jira") from it. Two entry points:
//
//   LoadFromFile(path)   — for on-disk fixtures under tests/fixtures/jira_backend/
//   LoadFromString(json) — for inline Bucket A tests
//
// Factory usage:
//   auto fixture = JiraFakeTrackerFixture::LoadFromFile("basic-grid.json");
//   auto client  = fixture.CreateClient();        // fresh per call — no state leak
//
// Fixture JSON schema: see docs/plans/active/deterministic-jira-test-backend.md § Slice 2.

#include "CachedTicketTypes.h"
#include "FakeTrackerClient.h"
#include "ITrackerConnectivity.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace smatchet_tests {

/// One scripted response to a FetchIssues call.
struct JiraFixtureFetch {
    bool FullSyncCompleted = true;
    std::string Warning;
    std::string FetchError;
    std::vector<CachedTicket> Tickets;
};

/// Immutable parsed fixture. Build via LoadFromFile / LoadFromString.
class JiraFakeTrackerFixture {
  public:
    /// Load fixture JSON from a file path. Throws std::runtime_error on parse failure.
    static JiraFakeTrackerFixture LoadFromFile(const std::string& path);

    /// Load fixture JSON from a string. Throws std::runtime_error on parse failure.
    static JiraFakeTrackerFixture LoadFromString(const std::string& json);

    /// Apply this fixture to an existing client (used by Bucket A direct-configure tests).
    void Configure(FakeTrackerClient& client) const;

    /// Create and configure a fresh FakeTrackerClient("Jira"). Fresh-per-call so factory
    /// invocations during TicketSyncService backend transitions don't share state.
    std::unique_ptr<FakeTrackerClient> CreateClient() const;

  private:
    static JiraFakeTrackerFixture ParseJson(const nlohmann::json& root);

    TrackerReachabilityProbeKind reachabilityKind_ = TrackerReachabilityProbeKind::AuthenticatedReachable;
    std::string reachabilityDiagnostic_;
    std::vector<JiraFixtureFetch> fetches_;
    // Scripted mutation replies (FIFO, same shape as FakeTrackerClient queues).
    std::vector<std::pair<bool, std::string>> updateIssueFieldsReplies_;
    std::vector<std::pair<bool, std::string>> createIssueReplies_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_JIRA_FAKE_TRACKER_FIXTURE_H
