#include "JiraChangelogDeltaPure.h"

#include <string>
#include <vector>

namespace JiraChangelogDeltaPure {

namespace {

/// String value at `key`, or empty when absent / non-string (mirrors JiraActivityFeed).
std::string JsonValueAsString(const nlohmann::json& node, const char* key) {
    if (!node.contains(key)) {
        return std::string();
    }
    const auto& v = node[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return std::string();
}

/// Human side of a from/to pair: prefer the display string, fall back to the raw value.
/// Unlike JiraActivityFeed there is no estimate-seconds special case — no salient role is an
/// estimate field, so the roster filter never lets one through.
std::string DisplaySide(const std::string& raw, const std::string& display) {
    return display.empty() ? raw : display;
}

/// Roster lookup by concrete field id; returns the matched role (for its label) or nullptr.
const smatchet::SalientFieldRole* FindRole(const std::string& id,
                                           const std::vector<smatchet::SalientFieldRole>& roster) {
    if (id.empty()) {
        return nullptr;
    }
    for (size_t i = 0; i < roster.size(); ++i) {
        if (roster[i].fieldId == id) {
            return &roster[i];
        }
    }
    return nullptr;
}

/// Keep rule for a history `created` timestamp: leading-16-char "YYYY-MM-DDTHH:MM" compare.
/// An empty `sinceIsoMinute` is the baseline (first) poll and keeps everything; either side
/// too short to carry a minute is kept (conservative — never silently drop a real change on a
/// malformed anchor or a truncated `created`).
bool CreatedSinceMinute(const std::string& created, const std::string& sinceIsoMinute) {
    if (sinceIsoMinute.empty()) {
        return true;
    }
    if (sinceIsoMinute.size() < 16) {
        return true;
    }
    if (created.size() < 16) {
        return true;
    }
    return created.substr(0, 16) >= sinceIsoMinute.substr(0, 16);
}

} // namespace

std::vector<JiraChangelogDelta>
DeltasFromIssueJson(const nlohmann::json& issue, const std::string& sinceIsoMinute,
                    const std::vector<smatchet::SalientFieldRole>& roster,
                    const std::string& selfAccountId) {
    std::vector<JiraChangelogDelta> out;
    if (!issue.is_object()) {
        return out;
    }
    const std::string key = JsonValueAsString(issue, "key");
    if (!issue.contains("changelog") || !issue["changelog"].is_object()) {
        return out;
    }
    const auto histories = issue["changelog"].value("histories", nlohmann::json::array());
    if (!histories.is_array()) {
        return out;
    }
    for (const auto& history : histories) {
        if (!history.is_object()) {
            continue;
        }
        std::string authorAccountId;
        std::string authorName;
        if (history.contains("author") && history["author"].is_object()) {
            authorAccountId = JsonValueAsString(history["author"], "accountId");
            authorName = JsonValueAsString(history["author"], "displayName");
        }
        if (!selfAccountId.empty() && authorAccountId == selfAccountId) {
            continue; // self-authored change — the monitor never notifies you of your own edit
        }
        const std::string created = JsonValueAsString(history, "created");
        if (!CreatedSinceMinute(created, sinceIsoMinute)) {
            continue;
        }
        const std::string entryId = JsonValueAsString(history, "id");
        const auto items = history.value("items", nlohmann::json::array());
        if (!items.is_array()) {
            continue;
        }
        for (const auto& item : items) {
            if (!item.is_object()) {
                continue;
            }
            const std::string fieldId = JsonValueAsString(item, "fieldId");
            const std::string field = JsonValueAsString(item, "field");
            const smatchet::SalientFieldRole* role = FindRole(fieldId, roster);
            if (role == nullptr) {
                role = FindRole(field, roster);
            }
            if (role == nullptr) {
                continue; // non-salient item — skip
            }
            JiraChangelogDelta delta;
            delta.summary.kind = smatchet::TicketChangeKind::Modified;
            delta.summary.issueId = key;
            delta.summary.changedFieldLabel = role->label;
            delta.summary.fromValue =
                DisplaySide(JsonValueAsString(item, "from"), JsonValueAsString(item, "fromString"));
            delta.summary.toValue =
                DisplaySide(JsonValueAsString(item, "to"), JsonValueAsString(item, "toString"));
            delta.summary.author = authorName;
            delta.changelogEntryId = entryId;
            delta.createdAt = created;
            out.push_back(std::move(delta));
        }
    }
    return out;
}

} // namespace JiraChangelogDeltaPure
