#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

struct TrackerConfig;

/// Multi-out payload for FetchProjectComponents (Jira-only) — the per-project
/// component list plus the parallel dropdown options, returned together as the
/// Result Ok value (replaces the prior pair of out-vector params).
struct TrackerProjectComponents {
    std::vector<TrackerComponent> Components;
    std::vector<TrackerFieldOption> Options;
};

class ITrackerFieldCatalog {
  public:
    virtual ~ITrackerFieldCatalog() = default;

    virtual Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& /*cfg*/,
                                                                              const std::string& /*projectKey*/) {
        return Result<TrackerFieldCatalogResult, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchFieldCatalog is not supported by this backend."));
    }

    virtual Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/) {
        return Result<std::unordered_map<std::string, bool>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchIssueEditMeta is not supported by this backend."));
    }

    virtual Result<TrackerProjectComponents, TrackerError> FetchProjectComponents(const TrackerConfig& /*cfg*/,
                                                                                  const std::string& /*projectKey*/) {
        return Result<TrackerProjectComponents, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchProjectComponents is not supported by this backend."));
    }

    // SMATCHET_DEVIATION(rule=duplication; reason=default not-supported stubs repeat one Err shape; owner=tracker-backend; revisit=2027-09-01)
    virtual Result<std::vector<TrackerFieldOption>, TrackerError>
    FetchIssueTransitions(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/) {
        return Result<std::vector<TrackerFieldOption>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchIssueTransitions is not supported by this backend."));
    }

    /// True when FetchIssueTransitions is implemented; callers skip the request (and its log) otherwise.
    virtual bool SupportsIssueTransitions() const { return false; }

    /// True when FetchIssueEditMeta's answer for `issueKeyOrId` can differ from another issue of the same
    /// project and issue type (a Done issue lists nothing editable; a commit row is read-only). Callers then
    /// fetch that issue's own edit metadata; when false one fetch per project and type stands for every
    /// issue, so a per-row fetch is skipped. Default false: only a client whose answer varies overrides it.
    virtual bool IssueEditMetaVariesPerIssue(const std::string& /*issueKeyOrId*/) const { return false; }
};
