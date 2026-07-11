#ifndef SMATCHET_TESTS_FAKE_GITHUB_FIXTURE_H
#define SMATCHET_TESTS_FAKE_GITHUB_FIXTURE_H

// FakeGitHubFixture — slice 1 of docs/plans/shipped/autonomous-debugging-no-creds.md.
//
// Header-only loader that:
//   1. Reads a GitHub GraphQL-search-shaped JSON file from tests/fixtures/github/<scenario>.json.
//   2. Maps the `nodes[]` array into CachedTicket vectors via the pure-logic
//      `MapGraphQlNodesToTickets` helper (from github-tracker-backend.md).
//   3. Scripts a `FakeTrackerClient("GitHub")` instance with those tickets
//      pre-loaded so doctest cases can drive the scenario surface without
//      HTTP, without cpr, without a PAT.
//
// Accepts the same two fixture shapes as the production `GitHubFixtureBackend`:
//   - Full GraphQL response: { "data": { "search": { "nodes": [...] } } }
//   - Minimal: { "nodes": [...] }
//
// The fixture's loader-error message is surfaced via `LoadError()` so tests
// can assert on a malformed-fixture path without inferring it from an empty
// ticket vector (which is a legitimate state when `nodes[]` is empty).

#include "FakeTrackerClient.h"
#include "GitHubIssueSearchMapping.h"
#include "LocalCacheManager.h" // CachedTicket

#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace smatchet_tests {

class FakeGitHubFixture {
  public:
    /// Load `fixturePath`; on success `Tickets()` + `ConfiguredClient()` are populated.
    /// On failure `LoadError()` carries a diagnostic and `Tickets()` is empty.
    /// `ownerHint` / `repoHint` are forwarded to `MapGraphQlNodesToTickets` for
    /// minimal fixtures that omit `repository.owner.login` / `repository.name`.
    /// `includePullRequests` mirrors the live `GitHubFetchPlan` flag.
    FakeGitHubFixture(const std::string& fixturePath, const std::string& ownerHint = std::string(),
                      const std::string& repoHint = std::string(), bool includePullRequests = true) {
        std::ifstream in(fixturePath, std::ios::binary);
        if (!in.is_open()) {
            loadError_ = std::string("Failed to open fixture file: ") + fixturePath;
            return;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string contents = ss.str();
        nlohmann::json parsed = nlohmann::json::parse(contents, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            loadError_ = std::string("Fixture JSON parse failed or root is not an object: ") + fixturePath;
            return;
        }
        const nlohmann::json* nodesPtr = nullptr;
        if (parsed.contains("data") && parsed["data"].is_object() && parsed["data"].contains("search") &&
            parsed["data"]["search"].is_object() && parsed["data"]["search"].contains("nodes")) {
            nodesPtr = &parsed["data"]["search"]["nodes"];
        } else if (parsed.contains("nodes")) {
            nodesPtr = &parsed["nodes"];
        }
        if (nodesPtr == nullptr || !nodesPtr->is_array()) {
            loadError_ = std::string("Fixture JSON missing 'nodes' array (expected either "
                                     "data.search.nodes or top-level nodes): ") +
                         fixturePath;
            return;
        }
        tickets_ = smatchet::github::MapGraphQlNodesToTickets(*nodesPtr, ownerHint, repoHint, includePullRequests);
    }

    const std::string& LoadError() const { return loadError_; }
    const std::vector<CachedTicket>& Tickets() const { return tickets_; }

    /// Build a `FakeTrackerClient` scripted with the fixture's tickets. Caller
    /// owns the returned client. Returns null when the fixture failed to load
    /// (check `LoadError()` for the reason).
    std::unique_ptr<FakeTrackerClient> ConfigureFakeClient() const {
        if (!loadError_.empty()) {
            return nullptr;
        }
        std::unique_ptr<FakeTrackerClient> client(new FakeTrackerClient(std::string("GitHub")));
        client->SetFetchIssuesResult(tickets_, /*fullSyncCompleted=*/true);
        // FetchIssuesForKeys returns the subset whose `id` matches one of the requested keys.
        // Tests that need a different policy override via FakeTrackerClient::SetFetchIssuesForKeysResult.
        client->SetFetchIssuesForKeysResult(true, tickets_);
        return client;
    }

  private:
    std::string loadError_;
    std::vector<CachedTicket> tickets_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_GITHUB_FIXTURE_H
