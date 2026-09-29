#pragma once
#include <string>
#include <utility>
#include <vector>

#include "SmatchetResult.h"
#include "TrackerError.h"
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

/// ListProjectsTyped's result: the visible projects, or why listing them failed.
using ProjectListResult = Result<std::vector<RemoteProject>, TrackerError>;

class ITrackerConnectivity {
  public:
    virtual ~ITrackerConnectivity() = default;

    virtual std::string GetTrackerType() const = 0;

    virtual TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) = 0;

    // Best-effort extract of a single project from a backend-specific query.
    // Returns "" when no project clause is present OR when multiple projects are referenced
    // (sentinel for the "ambiguous" case — callers must surface a picker).
    virtual std::string ExtractProjectFromQuery(const std::string& /*query*/) const { return ""; }

    // List projects visible to the current credentials; backends that support project enumeration
    // (Jira / Plane / GitHub / Linear) override this to drive the project picker. A failure keeps
    // its kind (Transport when the tracker is unreachable), so a caller can tell "offline" from
    // "no projects" and fall back to a saved list. Default: Ok and empty.
    virtual ProjectListResult ListProjectsTyped() { return ProjectListResult::Ok(std::vector<RemoteProject>()); }

    // Best-effort ListProjectsTyped: empty on any failure (for callers that only match against it).
    std::vector<RemoteProject> ListProjects() {
        ProjectListResult listed = ListProjectsTyped();
        return listed.has_value() ? std::move(listed.value()) : std::vector<RemoteProject>();
    }
};
