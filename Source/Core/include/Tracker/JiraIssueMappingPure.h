#ifndef SMATCHET_JIRA_ISSUE_MAPPING_PURE_H
#define SMATCHET_JIRA_ISSUE_MAPPING_PURE_H

// Slice 1 of deterministic-jira-test-backend — pure-logic helpers
// extracted from JiraIssueSearch.cpp (which pulls cpr) so the doctest rig and the
// fixture-backed fake can exercise Jira JSON normalization without HTTP. Mirrors the
// Plane (Slice 2 of autonomous-debugging-no-creds) and GitHub pure-mapping splits.
// Surfaces:
//   - BuildFetchFieldListsFromView: builds the Jira `fields` query param list and the
//     selected-fields subset from the active ViewsStore entry.
//   - AppendCachedTicketFromJiraSearchIssue: maps one Jira search-result issue JSON
//     to a CachedTicket appended onto a results vector. The comment-fetch callback is
//     injectable so tests can supply in-memory data without HTTP.

#include "CachedTicketTypes.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

struct ViewsStore;

namespace smatchet {
namespace jira {

/// The six Jira duration-in-seconds field ids — the single source of truth
/// that TrackerFieldValueUtils::IsTimeDurationField delegates to.
bool IsJiraDurationSecondsFieldKey(const std::string& fieldKey);

/// Result of matching a requested status against Jira's `transitions` array.
/// `id` is empty when no candidate was found. `usedNameFallback` is true when the
/// match came from the LAST-RESORT transition-name heuristic (not an exact status
/// id / `to.name`) — the caller logs a divergence warning in that case (this pure
/// unit is Logger-free by contract).
struct JiraTransitionMatch {
    std::string id;
    bool usedNameFallback = false;
    std::string transitionName; // populated only when usedNameFallback
    std::string toStatusName;   // the matched transition's actual to.name (for the warn)
};

/// Find the transition id that leads to the requested status. Priority, applied
/// GLOBALLY across all transitions (not per-transition — #670): (1) exact status
/// id, (2) exact `to.name` (case-insensitive), then only if NEITHER matched any
/// transition, (3) transition-name fallback (first match). A per-transition
/// priority could return an early transition whose *name* matched the requested
/// status ahead of a later transition that actually *leads* there → wrong status.
JiraTransitionMatch FindJiraTransitionId(const nlohmann::json& transitionsArray, const std::string& targetStatusId,
                                         const std::string& targetStatusName);

/// Fills `outFieldsList` (all fields to request from Jira) and `outSelectedFields`
/// (the subset to populate on CachedTicket) from the active view in `viewStore`.
/// Falls back to a sensible default set when no active view or no fields are configured.
void BuildFetchFieldListsFromView(const ViewsStore& viewStore, std::vector<std::string>& outFieldsList,
                                  std::vector<std::string>& outSelectedFields);

/// Maps one Jira search-result issue JSON object to a CachedTicket appended onto
/// `results`. Returns false (without appending) when the JSON is malformed.
/// `fetchIssueComments` is called lazily when the inline comment array is empty but
/// `total > 0`; tests pass an in-memory callback, production passes an HTTP fetch.
bool AppendCachedTicketFromJiraSearchIssue(
    const nlohmann::json& issue, const std::vector<std::string>& selectedFields,
    const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
    std::vector<CachedTicket>& results);

} // namespace jira
} // namespace smatchet

#endif // SMATCHET_JIRA_ISSUE_MAPPING_PURE_H
