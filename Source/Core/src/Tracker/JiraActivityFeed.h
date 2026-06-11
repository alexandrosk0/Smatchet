#pragma once
// Jira-private activity-feed helpers (Source/Core/src/Tracker/ — Jira-shaped JQL and
// changelog JSON stay behind JiraClient; only TrackerActivityEntry crosses out).
// The pure functions here are bucket-A tested (tests/Core/JiraActivityFeed.test.cpp).

#include "ITrackerActivity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace JiraActivityFeed {

/**
 * Discovery JQL for a user's activity window:
 * `(assignee WAS "<user>" OR reporter WAS "<user>") AND updated >= "<dayFrom>"
 *  AND updated <= "<dayTo>" [AND project = "<projectScope>"] ORDER BY updated DESC`.
 * Empty dayFrom/dayTo/projectScope drop the respective clause.
 */
std::string BuildActivityDiscoveryJql(const std::string& accountId, const std::string& dayFrom,
                                      const std::string& dayTo, const std::string& projectScope);

/**
 * Raw Jira estimate seconds -> "Xd Yh Zm" with an 8h workday. Zero units are omitted
 * ("0m" when everything is zero); input that is not a plain non-negative integer is
 * passed through unchanged (parse-failure passthrough).
 */
std::string FormatEstimateSeconds(const std::string& rawSeconds);

/**
 * One TrackerActivityEntry per changelog item authored by `accountId` whose history
 * timestamp day falls inside [dayFrom, dayTo] (leading-10-char lexicographic compare;
 * timestamps shorter than 10 chars are kept unconditionally; an empty bound is open).
 * `browseBase` is the normalized base URL — IssueUrl becomes `<browseBase>/browse/<key>`.
 */
std::vector<TrackerActivityEntry> EntriesFromIssueJson(const nlohmann::json& issue, const std::string& accountId,
                                                       const std::string& dayFrom, const std::string& dayTo,
                                                       const std::string& browseBase);

} // namespace JiraActivityFeed
