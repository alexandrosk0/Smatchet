#pragma once
// GitHub-private activity-feed helpers (Source/Core/src/Tracker/ — GitHub-shaped
// issue-event JSON stays behind GitHubClient; only TrackerActivityEntry crosses out).
// The pure functions here are bucket-A tested (tests/Core/GitHubActivityFeed.test.cpp).

#include "ITrackerActivity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace GitHubActivityFeed {

/**
 * One TrackerActivityEntry per element of a `/repos/{owner}/{repo}/issues/events`
 * array whose `actor.login` matches `accountId` and whose `created_at` day falls
 * inside [dayFrom, dayTo] (leading-10-char lexicographic compare; timestamps shorter
 * than 10 chars are kept unconditionally; an empty bound is open). Events with a
 * null/absent `issue` are skipped (no issue context to attribute). IssueKey is the
 * canonical `owner/repo#N`; ActionLabel is the event name; Details carries the
 * label / assignee / rename payload when present.
 */
std::vector<TrackerActivityEntry> EntriesFromEventsJson(const nlohmann::json& events, const std::string& accountId,
                                                        const std::string& dayFrom, const std::string& dayTo,
                                                        const std::string& owner, const std::string& repo);

/**
 * Early-stop rule for the page loop: `/issues/events` returns newest-first, so once
 * the last event of a page precedes `dayFrom` no later page can re-enter the window.
 * False on empty/malformed pages, empty `dayFrom`, or short/missing timestamps.
 */
bool PageEndsBeforeWindow(const nlohmann::json& events, const std::string& dayFrom);

} // namespace GitHubActivityFeed
