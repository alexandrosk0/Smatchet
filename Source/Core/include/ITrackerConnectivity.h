#pragma once
#include <string>
#include <vector>

#include "TrackerFieldSchema.h"

struct TrackerConfig;

enum class TrackerReachabilityProbeKind {
    AuthenticatedReachable,
    ReachableAuthOrConfigError,
    TransportDown,
    ServiceUnavailable,
};

struct TrackerReachabilityProbeResult {
    TrackerReachabilityProbeKind Kind = TrackerReachabilityProbeKind::TransportDown;
    std::string Diagnostic;
};

class ITrackerConnectivity {
  public:
    virtual ~ITrackerConnectivity() = default;

    virtual std::string GetTrackerType() const = 0;

    virtual TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) = 0;

    // Best-effort extract of a single project from a backend-specific query.
    // Returns "" when no project clause is present OR when multiple projects are referenced
    // (sentinel for the "ambiguous" case — callers must surface a picker).
    virtual std::string ExtractProjectFromQuery(const std::string& /*query*/) const { return ""; }

    // List projects visible to the current credentials. Default empty; backends that support
    // project enumeration (Jira / Plane / GitHub) override this to drive the project picker.
    virtual std::vector<RemoteProject> ListProjects() { return {}; }
};
