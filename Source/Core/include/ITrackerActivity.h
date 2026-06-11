#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

struct TrackerConfig;

/**
 * One row of a user's recent tracker activity in a backend-agnostic shape, so Jira
 * changelog items, Plane issue-history entries, and GitHub repo issue events can all
 * surface through a single type (see `ITrackerActivity::FetchUserActivity`).
 *
 * `Timestamp` is normalised to ISO-8601 (`YYYY-MM-DDTHH:MM:SS...`) at the adapter
 * boundary so consumers can sort and day-window by lexicographic comparison of the
 * leading 10 chars without parsing backend-native date formats.
 */
struct TrackerActivityEntry {
    std::string Timestamp;   // ISO-8601, lexicographically sortable
    std::string IssueKey;    // backend issue key the activity happened on (e.g. PROJ-123)
    std::string IssueUrl;    // browse URL for the issue (context-menu open-in-browser only)
    std::string Summary;     // issue summary at fetch time
    std::string ActionLabel; // short human label for what changed (e.g. "status", "assignee")
    std::string Details;     // free-form from -> to detail text (plain text, opaque to UI)
};

/**
 * Cross-thread progress counter for a `FetchUserActivity` run: the worker increments,
 * the UI thread polls for the loading bar. Non-copyable (atomics + shared identity).
 * `Total` may grow while a run discovers more issues; `Current <= Total` is not a
 * guaranteed invariant mid-update — treat both as monotonic hints, not exact state.
 */
struct TrackerActivityProgress {
    std::atomic<int> Current{0};
    std::atomic<int> Total{0};

    TrackerActivityProgress() = default;
    TrackerActivityProgress(const TrackerActivityProgress&) = delete;
    TrackerActivityProgress& operator=(const TrackerActivityProgress&) = delete;
};

/**
 * Narrow nullable backend role for user-activity + group lookups, reached via
 * `ITrackerBackend::Activity()` (nullptr when unsupported — same pattern as
 * `Collaboration()`). Backend-agnostic by invariant: no `Jira*` / `Plane*` /
 * `GitHub*` type may appear in this header.
 */
class ITrackerActivity {
  public:
    virtual ~ITrackerActivity() = default;

    /**
     * Recent activity authored by `accountId` between `dayFrom` and `dayTo` (inclusive,
     * `YYYY-MM-DD`). `projectScope` is a backend-agnostic scope hint — the source pane's
     * project key; backends with project-scoped queries (Jira) AND it into discovery,
     * naturally-scoped backends (Plane, GitHub) may ignore it. `progress` is updated from
     * the calling (worker) thread as issues are scanned. Long-running — never call on the
     * UI thread.
     */
    virtual Result<std::vector<TrackerActivityEntry>, TrackerError>
    FetchUserActivity(const TrackerConfig& /*cfg*/, const std::string& /*accountId*/, const std::string& /*dayFrom*/,
                      const std::string& /*dayTo*/, const std::string& /*projectScope*/,
                      TrackerActivityProgress& /*progress*/) {
        return Result<std::vector<TrackerActivityEntry>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchUserActivity is not supported by this backend."));
    }

    /// Members of a named backend group (for the group-roster cache).
    virtual Result<std::vector<TrackerUser>, TrackerError> FetchGroupMembers(const TrackerConfig& /*cfg*/,
                                                                             const std::string& /*groupName*/) {
        return Result<std::vector<TrackerUser>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchGroupMembers is not supported by this backend."));
    }

    /// Best-effort group names for a user (backends may return Ok with an empty list when
    /// the API withholds membership — e.g. Jira Cloud 403).
    virtual Result<std::vector<std::string>, TrackerError> FetchUserGroupNames(const TrackerConfig& /*cfg*/,
                                                                               const std::string& /*accountId*/) {
        return Result<std::vector<std::string>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchUserGroupNames is not supported by this backend."));
    }

    /// Cancel any in-flight `FetchUserActivity` and drop cached per-run state. Safe to
    /// call from any thread; default no-op for backends without run state.
    virtual void ClearUserActivity() {}
};
