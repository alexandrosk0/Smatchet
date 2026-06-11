// user-info-window.md Slice 4 — pure half of the GitHub activity feed, split out of
// GitHubActivityFeed.cpp (which defines GitHubClient members and so pulls cpr +
// the full GitHub client TU set) so the doctest rig links it standalone with only
// GitHubClientHelpers.cpp (mirrors the GitHubIssueSearchMapping convention).

#include "GitHubActivityFeed.h"

#include "GitHubClientHelpers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GitHubActivityFeed {

namespace {

std::string JsonString(const nlohmann::json& node, const char* key) {
    if (!node.is_object() || !node.contains(key)) {
        return std::string();
    }
    const auto& v = node[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return std::string();
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

/// Human detail for the event payloads that carry one: labeled/unlabeled (label),
/// assigned/unassigned (assignee), renamed (rename.from/to). Other events have no
/// payload worth surfacing — empty Details.
std::string EventDetails(const nlohmann::json& event) {
    if (event.contains("label") && event["label"].is_object()) {
        return JsonString(event["label"], "name");
    }
    if (event.contains("assignee") && event["assignee"].is_object()) {
        return JsonString(event["assignee"], "login");
    }
    if (event.contains("rename") && event["rename"].is_object()) {
        const std::string from = JsonString(event["rename"], "from");
        const std::string to = JsonString(event["rename"], "to");
        return (from.empty() ? std::string("(none)") : from) + " -> " + (to.empty() ? std::string("(none)") : to);
    }
    return std::string();
}

} // namespace

std::vector<TrackerActivityEntry> EntriesFromEventsJson(const nlohmann::json& events, const std::string& accountId,
                                                        const std::string& dayFrom, const std::string& dayTo,
                                                        const std::string& owner, const std::string& repo) {
    std::vector<TrackerActivityEntry> entries;
    if (!events.is_array()) {
        return entries;
    }
    for (const auto& event : events) {
        if (!event.is_object()) {
            continue;
        }
        std::string actorLogin;
        if (event.contains("actor") && event["actor"].is_object()) {
            actorLogin = JsonString(event["actor"], "login");
        }
        if (actorLogin != accountId) {
            continue;
        }
        const std::string created = JsonString(event, "created_at");
        if (!TimestampInWindow(created, dayFrom, dayTo)) {
            continue;
        }
        if (!event.contains("issue") || !event["issue"].is_object()) {
            continue; // no issue context to attribute the event to
        }
        const auto& issue = event["issue"];
        std::int64_t number = 0;
        if (issue.contains("number") && issue["number"].is_number_integer()) {
            number = issue["number"].get<std::int64_t>();
        }
        if (number <= 0) {
            continue;
        }
        TrackerActivityEntry entry;
        entry.Timestamp = created;
        entry.IssueKey = smatchet::github::FormatGitHubIssueKey(owner, repo, number);
        entry.IssueUrl = JsonString(issue, "html_url");
        entry.Summary = JsonString(issue, "title");
        entry.ActionLabel = JsonString(event, "event");
        entry.Details = EventDetails(event);
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool PageEndsBeforeWindow(const nlohmann::json& events, const std::string& dayFrom) {
    if (dayFrom.empty() || !events.is_array() || events.empty()) {
        return false;
    }
    const auto& last = events.back();
    if (!last.is_object()) {
        return false;
    }
    const std::string created = JsonString(last, "created_at");
    if (created.size() < 10) {
        return false;
    }
    return created.substr(0, 10) < dayFrom;
}

} // namespace GitHubActivityFeed
