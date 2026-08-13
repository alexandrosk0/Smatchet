#ifndef SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H
#define SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H

// Shared read-path base for the deterministic, production-resident fixture backends
// (GitHubFixtureBackend / PlaneFixtureBackend / LinearFixtureBackend). Each of those
// reads a JSON fixture from disk at construction, maps it through its own pure mapper,
// and then serves reads from the resulting CachedTickets — the mapping differs per
// backend, but everything downstream of it was byte-identical across all three (a
// ~1,800-token clone family in the DRY-pillar baseline, docs/adr/0015).
//
// What lives here is exactly the identical part: the role-interface accessors, the two
// read paths, and the fixture state they read from. What stays in each derived backend
// is what genuinely differs:
//
//   * the constructor + fixture->CachedTicket mapping (different JSON schema per backend);
//   * `GetTrackerType()`;
//   * `ProbeReachability()` — the diagnostic strings differ, and GitHub/Linear report
//     ServiceUnavailable on a load error where Plane does not;
//   * the mutation surface — Plane/Linear reject writes with a read-only error, while
//     GitHub logs a no-op and returns Ok, and their BuildFieldPayload results differ.
//
// This is a within-subsystem base for three backends that already share their role
// interfaces, not a helper spanning independent subsystems — the distinction the
// double-edged-DRY guardrail in docs/agent-rules/quality-pillars.md turns on.

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
namespace tracker_fixture {

/// Read-path scaffolding shared by the per-backend fixture backends. Abstract: a
/// derived class must still supply `GetTrackerType`, `ProbeReachability`, and the
/// mutation surface.
class TrackerFixtureBackendBase : public ITrackerBackend,
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

    /// Serves the tickets mapped at construction. A fixture that failed to load
    /// yields no rows plus the load diagnostic on every out-param.
    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr, std::string* outWarning = nullptr,
                                          TrackerError* outFetchErrorStructured = nullptr) override;

    /// Linear scan of the loaded tickets by id. Errs when the fixture failed to load.
    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;

    /// Fixtures carry no display-name catalog — the raw value is the display value.
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

    /// Fixtures are read-only by default: writes are rejected with an invalid-request
    /// error naming the backend ("<Type>FixtureBackend is read-only"), and BuildFieldPayload
    /// yields an empty object. A fixture that wants write semantics (GitHub logs the call and
    /// returns Ok so mutating scenarios can run) overrides these three.
    ///
    /// The three signatures below match every other ITrackerIssueMutations implementer verbatim
    /// because the interface fixes them — declaration-shape symmetry, not duplicated logic.
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-decl symmetry; owner=tracker; revisit=2026-12-31)
    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;
    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;
    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;

    /// Non-empty when the fixture failed to load. Caller (AppController) logs once.
    const std::string& LoadError() const { return loadError_; }

  protected:
    explicit TrackerFixtureBackendBase(std::string fixturePath);

    /// Open `fixturePath_`, read it, and bounded-parse it into `out`. The fixture path is
    /// env-var-selectable per backend (SMATCHET_TEST_*_BACKEND_FIXTURE), so the parse caps
    /// depth/nodes/bytes rather than trusting wherever it points. On any I/O or parse failure
    /// this sets `loadError_` and returns false, leaving the backend empty — the caller
    /// should simply return from its constructor.
    bool LoadFixtureJson(nlohmann::json& out);

    std::string fixturePath_;
    std::string loadError_;
    std::vector<CachedTicket> tickets_;
};

} // namespace tracker_fixture
} // namespace smatchet

#endif // SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H
