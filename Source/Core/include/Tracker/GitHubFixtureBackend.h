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

#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

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
class GitHubFixtureBackend : public ITrackerBackend,
                             public ITrackerIssueReader,
                             public ITrackerConnectivity,
                             public ITrackerIssueMutations {
  public:
    ITrackerIssueReader& Reader() override;
    ITrackerConnectivity& Connectivity() override;
    ITrackerFieldCatalog* FieldCatalog() override;
    ITrackerIssueMutations* Mutations() override;
    ITrackerCollaboration* Collaboration() override;
    ITrackerActivity* Activity() override;
    /// `fixturePath` must point to a JSON file shaped like the GraphQL search
    /// response — top-level `nodes[]` array of Issue/PullRequest objects.
    /// `ownerHint` / `repoHint` are used when nodes don't carry
    /// `repository.owner.login` / `repository.name` (i.e. minimal fixtures).
    /// `includePullRequests` mirrors the live `GitHubFetchPlan` flag — false
    /// drops PR rows during mapping.
    GitHubFixtureBackend(const std::string& fixturePath, const std::string& ownerHint, const std::string& repoHint,
                         bool includePullRequests);

    // SMATCHET_DEVIATION(rule=duplication; reason=backend API symmetry; owner=tracker; revisit=2026-12-31)
    std::string GetTrackerType() const override { return "GitHub"; }

    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr, std::string* outWarning = nullptr,
                                          TrackerError* outFetchErrorStructured = nullptr) override;

    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;

    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;

    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;

    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;

    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

    /// Diagnostic: load error message (empty when the fixture parsed cleanly).
    /// Surfaced via `FetchIssues`' `outFetchError` on every call when non-empty.
    const std::string& LoadError() const { return loadError_; }

  private:
    std::string fixturePath_;
    std::string ownerHint_;
    std::string repoHint_;
    bool includePullRequests_ = true;
    std::vector<CachedTicket> tickets_;
    std::string loadError_;
};

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_FIXTURE_BACKEND_H
