#ifndef SMATCHET_PLANE_FIXTURE_BACKEND_H
#define SMATCHET_PLANE_FIXTURE_BACKEND_H

// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — deterministic
// in-process Plane backend. Reads a JSON fixture from disk at construction
// time, maps `list_response.results` through PlaneIssueMappingPure, and serves
// `FetchIssues` from the resulting CachedTickets. No HTTP, no network, no
// credentials. Production-resident (compiled into Source/Core) so it can be
// instantiated by `AppController::Initialize` when
// SMATCHET_TEST_PLANE_BACKEND_FIXTURE=<path> is set in the env.
// Mirrors slice 1's `JiraFixtureBackend` (not yet landed) + slice 4's
// `StubAiClient` (already shipped via `AiClientFactory::SetTestOverride`). The
// equivalent test-only fixture loader for the doctest rig lives at
// `tests/support/FakePlaneFixture.h` and shares the same JSON schema.

#include "ITrackerBackend.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "PlaneIssueMappingPure.h"

#include <memory>
#include <string>
#include <vector>

class ITrackerBackendFactory;
class ITrackerFieldCatalog;
class ITrackerCollaboration;

namespace smatchet {
namespace plane {

/// Read-only backend backed by a fixture JSON file.
class PlaneFixtureBackend : public ITrackerBackend,
                            public ITrackerIssueReader,
                            public ITrackerConnectivity,
                            public ITrackerIssueMutations {
  public:
    /// Construct from a fixture file path. On any I/O or JSON-parse error the
    /// constructor leaves the backend empty (`FetchIssues` returns no rows + a
    /// diagnostic in `outFetchError`). The detailed error message is also
    /// surfaced via `LoadError()` for one-time logging by the caller.
    ITrackerIssueReader& Reader() override;
    ITrackerConnectivity& Connectivity() override;
    ITrackerFieldCatalog* FieldCatalog() override;
    ITrackerIssueMutations* Mutations() override;
    ITrackerCollaboration* Collaboration() override;
    ITrackerActivity* Activity() override;

    explicit PlaneFixtureBackend(const std::string& fixturePath);

    std::string GetTrackerType() const override { return "Plane"; }

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

/// Backend factory that always returns a PlaneFixtureBackend bound to the
/// given fixture path, regardless of the requested tracker-type string. Used
/// by `AppController::Initialize` when SMATCHET_TEST_PLANE_BACKEND_FIXTURE is
/// set so the env hook short-circuits the default factory entirely.
std::unique_ptr<ITrackerBackendFactory> MakePlaneFixtureBackendFactory(const std::string& fixturePath);

} // namespace plane
} // namespace smatchet

#endif // SMATCHET_PLANE_FIXTURE_BACKEND_H
