#ifndef SMATCHET_GITHUB_FIXTURE_BACKEND_H
#define SMATCHET_GITHUB_FIXTURE_BACKEND_H

// Slice 1 of docs/plans/shipped/autonomous-debugging-no-creds.md — production-resident
// fixture-backed GitHub `ITrackerBackend` that reads a GraphQL-search JSON
// payload from disk instead of touching the network. Enables zero-credentials
// scenario replay (`SMATCHET_TEST_GITHUB_BACKEND_FIXTURE=<path>` env hook in
// `AppController::Initialize`).
// Implementation reuses the pure-logic mapper from
// `GitHubIssueSearchMapping.{h,cpp}` (github-tracker-backend.md), so
// the deterministic backend exercises the same JSON-to-CachedTicket path as
// the live GraphQL fetcher — only the HTTP source is replaced.
// Write paths (`UpdateField`, `CreateIssue`, etc.) succeed as no-ops with a
// structured log line so debug scenarios that drive UI writes against the
// fake don't crash. The fixture is read-only by design — debug-detective
// scenarios that mutate are out-of-scope for slice 1.

#include "Tracker/TrackerFixtureBackendBase.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class ITrackerFieldCatalog;
class ITrackerCollaboration;

namespace smatchet {
namespace github {

/// Production-resident fixture-backed GitHub backend. Loads the fixture file
/// once at construction; subsequent `FetchIssues` calls return the cached
/// vector. Reachability probe always returns `AuthenticatedReachable` so the
/// UI doesn't show a disconnect banner during fixture-driven scenarios.
class GitHubFixtureBackend : public smatchet::tracker_fixture::TrackerFixtureBackendBase {
  public:
    /// `fixturePath` must point to a JSON file shaped like the GraphQL search
    /// response — top-level `nodes[]` array of Issue/PullRequest objects.
    /// `ownerHint` / `repoHint` are used when nodes don't carry
    /// `repository.owner.login` / `repository.name` (i.e. minimal fixtures).
    /// `includePullRequests` mirrors the live `GitHubFetchPlan` flag — false
    /// drops PR rows during mapping.
    GitHubFixtureBackend(const std::string& fixturePath, const std::string& ownerHint, const std::string& repoHint,
                         bool includePullRequests);

    std::string GetTrackerType() const override { return "GitHub"; }

    // The per-backend override block below (reachability probe + the three mutation entry points)
    // is declaration-shape symmetry with the other tracker backends, not shareable logic: the
    // signatures are fixed by ITrackerConnectivity / ITrackerIssueMutations, and GitHub's write
    // bodies deliberately differ from the read-only default in TrackerFixtureBackendBase.
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-decl symmetry; owner=tracker; revisit=2026-12-31)
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;

    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;

    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;

    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;

  private:
    std::string ownerHint_;
    std::string repoHint_;
    bool includePullRequests_ = true;
};

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_FIXTURE_BACKEND_H
