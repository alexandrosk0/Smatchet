#pragma once
// Plane-private activity-feed helpers (Source/Core/src/Tracker/ — Plane-shaped
// activity JSON stays behind PlaneClient; only TrackerActivityEntry crosses out).
// The pure functions here are bucket-A tested (tests/Core/PlaneActivityFeed.test.cpp).

#include "ITrackerActivity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace PlaneActivityFeed {

/**
 * One TrackerActivityEntry per element of a Plane `/activities/` `results` array
 * whose actor matches `accountId` and whose `created_at` day falls inside
 * [dayFrom, dayTo] (leading-10-char lexicographic compare; timestamps shorter than
 * 10 chars are kept unconditionally; an empty bound is open). `actor` is accepted
 * as either a UUID string or an object carrying an `id` member. ActionLabel is the
 * activity's `field` (falling back to `verb`, then "updated"); Details is the
 * `old_value -> new_value` pair with "(none)" placeholders.
 */
std::vector<TrackerActivityEntry> EntriesFromActivitiesJson(const nlohmann::json& results, const std::string& accountId,
                                                            const std::string& dayFrom, const std::string& dayTo,
                                                            const std::string& issueKey,
                                                            const std::string& issueSummary,
                                                            const std::string& issueUrl);

/**
 * Discovery skip rule: a work item whose `updated_at` day precedes `dayFrom` cannot
 * carry in-window activities (activity timestamps never exceed the item's
 * updated_at), so its per-issue activities scan is skipped. Short/missing
 * timestamps and an empty `dayFrom` always scan.
 */
bool WorkItemWorthScanning(const std::string& updatedAtIso, const std::string& dayFrom);

} // namespace PlaneActivityFeed
