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

#include "Tracker/TrackerFixtureBackendBase.h"

#include <nlohmann/json.hpp>

// Below is per-backend declaration shell only — shared behaviour lives in TrackerFixtureBackendBase.
// SMATCHET_DEVIATION(rule=duplication; reason=fixture decl shell; owner=tracker; revisit=2026-12-31)
#include <memory>
#include <string>
#include <vector>

class ITrackerBackendFactory;
class ITrackerFieldCatalog;
class ITrackerCollaboration;

namespace smatchet {
namespace linear {

/// Read-only Linear backend backed by a fixture JSON file, loaded once at construction.
class LinearFixtureBackend : public smatchet::tracker_fixture::TrackerFixtureBackendBase {
  public:
    /// `fixturePath` must be JSON shaped like the GraphQL `issues` response
    /// (`{"data":{"issues":{"nodes":[...]}}}`) or a minimal `{"nodes":[...]}`. On any I/O or
    /// parse error the backend stays empty and the message surfaces via `LoadError()`.
    explicit LinearFixtureBackend(const std::string& fixturePath);

    std::string GetTrackerType() const override { return "Linear"; }

    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
};

/// Backend factory that always returns a LinearFixtureBackend bound to the given
/// fixture path, regardless of the requested tracker-type string. Used by
/// `AppController::Initialize` when SMATCHET_TEST_LINEAR_BACKEND_FIXTURE is set
/// so the env hook short-circuits the default factory entirely.
std::unique_ptr<ITrackerBackendFactory> MakeLinearFixtureBackendFactory(const std::string& fixturePath);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_FIXTURE_BACKEND_H
