#ifndef SMATCHET_LINEAR_FIXTURE_BACKEND_H
#define SMATCHET_LINEAR_FIXTURE_BACKEND_H

// Slice 4 of docs/plans/linear-tracker-backend.md — deterministic
// in-process Linear backend. Reads a JSON fixture from disk at construction,
// maps the issue `nodes` array through LinearIssueMappingPure, and serves
// `FetchIssues` from the resulting CachedTickets. No HTTP, no network, no API
// key. Production-resident (compiled into Source/Core) so it can be installed
// by `AppController::Initialize` when SMATCHET_TEST_LINEAR_BACKEND_FIXTURE=<path>
// is set in the env — the zero-credentials scenario-replay hook that the
// GitHub/Plane fixtures already provide for their backends.
// Reuses the same pure mapper (`MapLinearIssueNodesToTickets`) as the live
// GraphQL fetcher, so the fake exercises the identical JSON-to-CachedTicket path
// — only the HTTP source is replaced. Read-only by design: write paths return a
// structured "read-only" error rather than mutating (mirrors PlaneFixtureBackend).

#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <nlohmann/json.hpp>

// SMATCHET_DEVIATION(rule=duplication; reason=backend API symmetry; owner=tracker; revisit=2026-12-31)
#include <memory>
#include <string>
#include <vector>

class ITrackerBackendFactory;
class ITrackerFieldCatalog;
class ITrackerCollaboration;

namespace smatchet {
namespace linear {

/// Read-only Linear backend backed by a fixture JSON file. Loads once at
/// construction; `FetchIssues` returns the cached vector. Reachability always
/// reports `AuthenticatedReachable` so the UI shows no disconnect banner during
/// fixture-driven scenarios (matches GitHub/Plane fixtures).
class LinearFixtureBackend : public ITrackerBackend,
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

    /// `fixturePath` must point to a JSON file shaped like the GraphQL `issues`
    /// response — `{ "data": { "issues": { "nodes": [...] } } }` — or a minimal
    /// `{ "nodes": [...] }`. On any I/O or JSON-parse error the backend stays
    /// empty (`FetchIssues` returns no rows + a diagnostic in `outFetchError`);
    /// the detailed message is also surfaced via `LoadError()`.
    explicit LinearFixtureBackend(const std::string& fixturePath);

    std::string GetTrackerType() const override { return "Linear"; }

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
                                    const std::string& value) const override {
        (void)fieldId;
        (void)field;
        return value;
    }

    /// Non-empty when the fixture failed to load. Caller (AppController) logs once.
    const std::string& LoadError() const { return loadError_; }

  private:
    std::string fixturePath_;
    std::string loadError_;
    std::vector<CachedTicket> tickets_;
};

/// Backend factory that always returns a LinearFixtureBackend bound to the given
/// fixture path, regardless of the requested tracker-type string. Used by
/// `AppController::Initialize` when SMATCHET_TEST_LINEAR_BACKEND_FIXTURE is set
/// so the env hook short-circuits the default factory entirely.
std::unique_ptr<ITrackerBackendFactory> MakeLinearFixtureBackendFactory(const std::string& fixturePath);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_FIXTURE_BACKEND_H
