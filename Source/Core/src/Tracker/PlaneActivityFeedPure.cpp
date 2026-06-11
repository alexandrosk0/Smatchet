// user-info-window.md Slice 4 — pure half of the Plane activity feed, split out of
// PlaneActivityFeed.cpp (which defines PlaneClient members and so pulls cpr +
// the full Plane client TU set) so the doctest rig links it standalone
// (mirrors the PlaneIssueMappingPure convention).

#include "PlaneActivityFeed.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PlaneActivityFeed {

namespace {

// Local copy of the production JsonFieldToString helper (defined in PlaneClient.cpp
// inside the smatchet::plane_detail namespace). Replicated here to keep this TU
// free of the production internal header (which pulls cpr via PlaneClient_Internal.h).
std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return std::string();
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<std::int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? std::string("true") : std::string("false");
    return v.dump();
}

/// Day-window keep rule: leading-10-char compare; short/missing timestamps kept
/// unconditionally; an empty bound is open on that side.
bool TimestampInWindow(const std::string& ts, const std::string& dayFrom, const std::string& dayTo) {
    if (ts.size() < 10) {
        return true;
    }
    const std::string day = ts.substr(0, 10);
    if (!dayFrom.empty() && day < dayFrom) {
        return false;
    }
    if (!dayTo.empty() && day > dayTo) {
        return false;
    }
    return true;
}

/// Plane serialises `actor` as either a bare UUID string or an expanded object
/// carrying an `id` member — accept both.
std::string ActorAccountId(const nlohmann::json& activity) {
    if (!activity.contains("actor")) {
        return std::string();
    }
    const auto& actor = activity["actor"];
    if (actor.is_string()) {
        return actor.get<std::string>();
    }
    if (actor.is_object()) {
        return JsonFieldToString(actor, "id");
    }
    return std::string();
}

} // namespace

std::vector<TrackerActivityEntry> EntriesFromActivitiesJson(const nlohmann::json& results, const std::string& accountId,
                                                            const std::string& dayFrom, const std::string& dayTo,
                                                            const std::string& issueKey,
                                                            const std::string& issueSummary,
                                                            const std::string& issueUrl) {
    std::vector<TrackerActivityEntry> entries;
    if (!results.is_array()) {
        return entries;
    }
    for (const auto& activity : results) {
        if (!activity.is_object()) {
            continue;
        }
        if (ActorAccountId(activity) != accountId) {
            continue;
        }
        const std::string created = JsonFieldToString(activity, "created_at");
        if (!TimestampInWindow(created, dayFrom, dayTo)) {
            continue;
        }
        std::string field = JsonFieldToString(activity, "field");
        if (field.empty()) {
            field = JsonFieldToString(activity, "verb");
        }
        if (field.empty()) {
            field = "updated";
        }
        const std::string from = JsonFieldToString(activity, "old_value");
        const std::string to = JsonFieldToString(activity, "new_value");
        TrackerActivityEntry entry;
        entry.Timestamp = created;
        entry.IssueKey = issueKey;
        entry.IssueUrl = issueUrl;
        entry.Summary = issueSummary;
        entry.ActionLabel = field;
        entry.Details =
            (from.empty() ? std::string("(none)") : from) + " -> " + (to.empty() ? std::string("(none)") : to);
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool WorkItemWorthScanning(const std::string& updatedAtIso, const std::string& dayFrom) {
    if (dayFrom.empty() || updatedAtIso.size() < 10) {
        return true;
    }
    return updatedAtIso.substr(0, 10) >= dayFrom;
}

} // namespace PlaneActivityFeed
