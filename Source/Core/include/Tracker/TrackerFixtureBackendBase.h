#ifndef SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H
#define SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H

// Shared read path for the production-resident fixture backends (GitHub / Plane / Linear).
// Each reads a JSON fixture at construction and maps it through its own pure mapper; the
// mapping differs per backend, but everything downstream of it was byte-identical across all
// three (a ~1,800-token clone family in the DRY-pillar baseline, docs/adr/0015). That identical
// part — role accessors, both read paths, and the fixture state — lives here.
// Each backend still owns its fixture mapping, GetTrackerType, ProbeReachability, and (GitHub
// only) write semantics. A within-subsystem base for three backends that already share their
// role interfaces, not a helper spanning independent subsystems.

#include "ITrackerBackend.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerConnectivity.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
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

    /// Read-only by default: writes are rejected as invalid-request naming the backend
    /// ("<Type>FixtureBackend is read-only") and BuildFieldPayload yields an empty object.
    /// GitHub overrides all three to log a no-op and return Ok so mutating scenarios run.
    /// The signatures are fixed by ITrackerIssueMutations, hence identical everywhere.
    // SMATCHET_DEVIATION(rule=duplication; reason=ITrackerIssueMutations override declarations every fixture backend repeats; owner=tracker; revisit=never)
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
    /// env-var-selectable per backend (SMATCHET_TEST_*_BACKEND_FIXTURE), so the read stops at the
    /// parse's byte cap and the parse caps depth/nodes rather than trusting wherever it points.
    /// On any I/O, size or parse failure this sets `loadError_` and returns false, leaving the
    /// backend empty — the caller should simply return from its constructor.
    bool LoadFixtureJson(nlohmann::json& out);

    std::string fixturePath_;
    std::string loadError_;
    std::vector<CachedTicket> tickets_;
};

/// Builds the fixture backend for a path; the factory below calls it once per Create.
using FixtureBackendMaker = std::function<std::unique_ptr<TrackerFixtureBackendBase>(const std::string& fixturePath)>;

/// A backend factory that serves `fixturePath` whatever tracker type is requested, so the
/// SMATCHET_TEST_*_BACKEND_FIXTURE env hook short-circuits the default factory. `backendName`
/// ("Plane", "Linear") is the type served and the prefix of its log lines; `make` builds the backend.
std::unique_ptr<ITrackerBackendFactory> MakeFixtureBackendFactory(std::string fixturePath, std::string backendName,
                                                                  FixtureBackendMaker make);

/// MakeFixtureBackendFactory for a `Backend` constructible from the fixture path alone.
template <typename Backend>
std::unique_ptr<ITrackerBackendFactory> MakeFixtureBackendFactoryFor(std::string fixturePath, std::string backendName) {
    return MakeFixtureBackendFactory(std::move(fixturePath), std::move(backendName),
                                     [](const std::string& path) -> std::unique_ptr<TrackerFixtureBackendBase> {
                                         return std::make_unique<Backend>(path);
                                     });
}

} // namespace tracker_fixture
} // namespace smatchet

#endif // SMATCHET_TRACKER_FIXTURE_BACKEND_BASE_H
