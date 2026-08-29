// Slice 1 of deterministic-jira-test-backend — pure-logic helpers
// extracted from the anonymous namespace of JiraIssueSearch.cpp. No cpr, no Logger,
// no HTTP. See JiraIssueMappingPure.h for the public contract.

#include "JiraIssueMappingPure.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerFieldValueParser.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace jira {

namespace {

std::string TransitionFieldToString(const nlohmann::json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    return std::string();
}

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

JiraTransitionMatch FindJiraTransitionId(const nlohmann::json& transitionsArray, const std::string& targetStatusId,
                                         const std::string& targetStatusName) {
    JiraTransitionMatch fallback; // first transition-name match; used only if no exact match exists
    if (!transitionsArray.is_array()) {
        return JiraTransitionMatch();
    }
    for (const auto& transition : transitionsArray) {
        if (!transition.is_object()) {
            continue;
        }
        std::string thisId;
        if (transition.contains("id")) {
            thisId = TransitionFieldToString(transition["id"]);
        }
        if (thisId.empty()) {
            continue;
        }

        const std::string transitionName = transition.value("name", std::string());
        std::string toStatusId;
        std::string toStatusName;
        if (transition.contains("to") && transition["to"].is_object()) {
            const auto& to = transition["to"];
            if (to.contains("id")) {
                toStatusId = TransitionFieldToString(to["id"]);
            }
            toStatusName = to.value("name", std::string());
        }

        // Pass-1 priority (global): exact status id or exact to.name wins outright.
        if ((!targetStatusId.empty() && toStatusId == targetStatusId) ||
            (!targetStatusName.empty() && IEquals(toStatusName, targetStatusName))) {
            JiraTransitionMatch m;
            m.id = thisId;
            return m;
        }
        // Pass-2 candidate: remember the FIRST transition-name match, do NOT return
        // yet — a later transition may still be an exact status match.
        if (fallback.id.empty() && !targetStatusName.empty() && IEquals(transitionName, targetStatusName)) {
            fallback.id = thisId;
            fallback.usedNameFallback = true;
            fallback.transitionName = transitionName;
            fallback.toStatusName = toStatusName;
        }
    }
    return fallback;
}

void BuildFetchFieldListsFromView(const ViewsStore& viewStore, std::vector<std::string>& outFieldsList,
                                  std::vector<std::string>& outSelectedFields) {
    outFieldsList = std::vector<std::string>{"summary",   "description", "status",  "assignee", "priority",
                                             "issuetype", "parent",      "comment", "changelog"};
    std::unordered_set<std::string> seenFields(outFieldsList.begin(), outFieldsList.end());
    outSelectedFields.clear();
    std::unordered_set<std::string> seenSelectedFields;

    const ViewDefinition* activeViewDef = nullptr;
    auto vIt = std::find_if(viewStore.Views.begin(), viewStore.Views.end(),
                            [&](const ViewDefinition& view) { return view.Id == viewStore.ActiveViewId; });
    if (vIt != viewStore.Views.end()) {
        activeViewDef = &(*vIt);
    }
    if (!activeViewDef && !viewStore.Views.empty()) {
        activeViewDef = &viewStore.Views.front();
    }
    if (activeViewDef) {
        for (const auto& rawField : activeViewDef->Fields) {
            const std::string field = TrimCopy(rawField);
            if (field.empty()) {
                continue;
            }
            if (field == "history") {
                if (seenSelectedFields.insert(field).second) {
                    outSelectedFields.push_back(field);
                }
                continue;
            }
            if (!seenSelectedFields.insert(field).second) {
                continue;
            }
            if (seenFields.insert(field).second) {
                outFieldsList.push_back(field);
                outSelectedFields.push_back(field);
            } else {
                outSelectedFields.push_back(field);
            }
        }
    }
    if (outSelectedFields.empty()) {
        const char* kBasicDefaults[] = {
            "summary", "assignee", "priority", "status", "created", "updated",
        };
        for (const char* basicId : kBasicDefaults) {
            if (seenFields.insert(basicId).second) {
                outFieldsList.push_back(basicId);
            }
            if (seenSelectedFields.insert(basicId).second) {
                outSelectedFields.emplace_back(basicId);
            }
        }
    }
}

bool IsJiraDurationSecondsFieldKey(const std::string& fieldKey) {
    return fieldKey == "timeoriginalestimate" || fieldKey == "timeestimate" || fieldKey == "timespent" ||
           fieldKey == "aggregatetimeoriginalestimate" || fieldKey == "aggregatetimeestimate" ||
           fieldKey == "aggregatetimespent";
}

namespace {

std::string StringifyJiraFieldForGrid(const nlohmann::json& v) {
    if (v.is_object() || v.is_array()) {
        return v.dump();
    }
    return NormalizeTrackerFieldValue(v);
}

bool IsJiraTimetrackingFieldKey(const std::string& fieldKey) {
    return fieldKey == "timetracking" || fieldKey == "aggregatetimetracking";
}

bool IsJiraAttachmentFieldKey(const std::string& fieldKey) {
    return fieldKey == "attachment" || fieldKey == "attachments";
}

// Resolve the `comment` field, optionally fetching the full comment list when the
// search payload only carried a total count.
std::string
ResolveJiraCommentField(const nlohmann::json& rawValue, const std::string& ticketId,
                        const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments) {
    const auto& commentObj = rawValue;
    const auto& commentsArray = commentObj["comments"];
    const int totalComments = commentObj.value("total", static_cast<int>(commentsArray.size()));
    if (commentsArray.is_array() && !commentsArray.empty()) {
        return ParseComments(commentsArray);
    }
    if (totalComments > 0) {
        nlohmann::json fetchedComments = nlohmann::json::array();
        if (fetchIssueComments(ticketId, fetchedComments) && fetchedComments.is_array()) {
            return ParseComments(fetchedComments);
        }
        return ParseComments(commentsArray);
    }
    return std::string();
}

std::string ResolveJiraDurationSecondsField(const nlohmann::json& rawValue) {
    long long seconds = 0;
    if (rawValue.is_number_integer()) {
        seconds = rawValue.get<long long>();
    } else if (rawValue.is_number_unsigned()) {
        seconds = static_cast<long long>(rawValue.get<unsigned long long>());
    }
    return FormatWorkDurationFromSeconds(seconds);
}

// Map a single present field key onto its grid string, dispatching by special-cased
// field family. `comment` may trigger a lazy fetch via `fetchIssueComments`.
void MapJiraPresentField(const std::string& fieldKey, const nlohmann::json& rawValue, const std::string& ticketId,
                         const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
                         CachedTicket& ticket) {
    if (fieldKey == "comment" && rawValue.is_object() && rawValue.contains("comments")) {
        ticket.fieldValues[fieldKey] = ResolveJiraCommentField(rawValue, ticketId, fetchIssueComments);
        // issue-comments PR-B — populate the unified numeric `comments` column (plural, distinct
        // from the `comment` display blob above) so the shared comments cell renders a count.
        const auto& commentsArray = rawValue["comments"];
        ticket.fieldValues["comments"] =
            std::to_string(rawValue.value("total", static_cast<int>(commentsArray.size())));
    } else if (IsJiraTimetrackingFieldKey(fieldKey)) {
        ticket.fieldValues[fieldKey] =
            rawValue.is_object() ? FormatTrackerTimetrackingDisplay(rawValue) : NormalizeTrackerFieldValue(rawValue);
    } else if (IsJiraAttachmentFieldKey(fieldKey)) {
        ticket.fieldValues[fieldKey] = StringifyJiraFieldForGrid(rawValue);
    } else if (IsJiraDurationSecondsFieldKey(fieldKey)) {
        ticket.fieldValues[fieldKey] = ResolveJiraDurationSecondsField(rawValue);
    } else {
        ticket.fieldValues[fieldKey] = NormalizeTrackerFieldValue(rawValue);
    }
    if (rawValue.is_object()) {
        // nlohmann's value-with-default overload throws type_error.302 when the type
        // member exists but is not a string, since the default only applies to a missing
        // key. Look it up and type-check it first so malformed ADF cannot throw.
        const nlohmann::json::const_iterator typeIt = rawValue.find("type");
        if (typeIt != rawValue.end() && typeIt->is_string() && typeIt->get_ref<const std::string&>() == "doc") {
            ticket.fieldRichValues[fieldKey] = rawValue.dump();
        }
    }
}

void MapJiraWatchersField(const nlohmann::json& issueFields, CachedTicket& ticket) {
    if (issueFields.contains("watchers")) {
        ticket.fieldValues["watchers"] = StringifyJiraFieldForGrid(issueFields["watchers"]);
    } else if (issueFields.contains("watches")) {
        ticket.fieldValues["watchers"] = StringifyJiraFieldForGrid(issueFields["watches"]);
    } else {
        ticket.fieldValues["watchers"] = std::string();
    }
}

} // namespace

bool AppendCachedTicketFromJiraSearchIssue(
    const nlohmann::json& issue, const std::vector<std::string>& selectedFields,
    const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
    std::vector<CachedTicket>& results) {
    try {
        if (!issue.is_object()) {
            LOG_WARN("Jira issue is not a JSON object; skipping unmappable row");
            return false;
        }
        CachedTicket ticket;
        ticket.id = JsonGetStringIfString(issue, "key");
        // A row-level mapping failure should fire ONLY on structurally unmappable
        // issues. Jira identifies an issue by "key" (and/or "id"); with neither, the
        // row has no stable identity and cannot be reconciled downstream — reject it.
        if (ticket.id.empty()) {
            const std::string fallbackId = JsonGetStringIfString(issue, "id");
            if (fallbackId.empty()) {
                LOG_WARN("Jira issue has neither key nor id; skipping unmappable row");
                return false;
            }
            ticket.id = fallbackId;
        }

        nlohmann::json issueFields = issue.value("fields", nlohmann::json::object());

        for (const auto& fieldKey : selectedFields) {
            if (fieldKey == "history") {
                // Degrade the history column to empty on a malformed/explicit-null
                // changelog rather than dropping the whole row (#943). nlohmann's
                // value-with-default throws type_error.302 when "changelog" exists but
                // is not an object (e.g. explicit null), so type-check before reading.
                const nlohmann::json::const_iterator changelogIt = issue.find("changelog");
                if (changelogIt == issue.end() || !changelogIt->is_object()) {
                    if (changelogIt != issue.end() && !changelogIt->is_null()) {
                        LOG_WARN("Jira issue %s has a malformed changelog; history column degraded to empty",
                                 ticket.id.c_str());
                    } else if (changelogIt != issue.end()) {
                        LOG_WARN("Jira issue %s has an explicit-null changelog; history column degraded to empty",
                                 ticket.id.c_str());
                    }
                    ticket.fieldValues["history"] = std::string();
                    continue;
                }
                const nlohmann::json histories = changelogIt->value("histories", nlohmann::json::array());
                ticket.fieldValues["history"] = ParseChangelog(histories);
                continue;
            }
            if (fieldKey == "watchers") {
                MapJiraWatchersField(issueFields, ticket);
                continue;
            }
            if (issueFields.contains(fieldKey)) {
                MapJiraPresentField(fieldKey, issueFields[fieldKey], ticket.id, fetchIssueComments, ticket);
            } else if (ticket.fieldValues.find(fieldKey) == ticket.fieldValues.end()) {
                // Don't null a value a sibling field's handler already computed. The numeric
                // `comments` count is derived from the `comment` field (MapJiraPresentField),
                // and Jira's payload carries no literal `comments` key — so this absent-field
                // path would otherwise clobber the count depending on selected-field order.
                ticket.fieldValues[fieldKey] = std::string();
            }
        }
        // The comment blob is mapped OUTSIDE the selectedFields loop, and deliberately so.
        // BuildFetchFieldListsFromView always puts "comment" on the wire (outFieldsList), but it
        // only lands in outSelectedFields when the ACTIVE VIEW carries it as a column — and
        // EraseCatalogLegacyCommentField drops `comment` from the picker, so no view can. Keying
        // the mapping off selectedFields therefore fetched the payload on every sync and threw it
        // away, leaving both the Comments-cell hover tooltip AND its numeric count empty.
        // MapJiraPresentField derives fieldValues["comments"] (the count) from this same branch,
        // so this must run AFTER the loop: the loop's absent-key path blanks "comments" when it is
        // a selected column (Jira's payload has no literal `comments` key) and this has to win.
        // The find() guard keeps it idempotent for a legacy view that still lists `comment`, so
        // ResolveJiraCommentField's lazy top-up cannot fire twice for one row. A NO-OP fetch is
        // passed instead of the live callback: ResolveJiraCommentField falls back to a per-issue
        // HTTP call when the inline array is empty but total > 0, and running that unconditionally
        // would put a blocking network fetch per row inside the sync page loop ("NO per-row
        // network during sync"). Rows hitting that payload shape keep their count here and get
        // the blob from the grid cell's one-shot first-hover fetch instead.
        if (issueFields.contains("comment") && ticket.fieldValues.find("comment") == ticket.fieldValues.end()) {
            const std::function<bool(const std::string&, nlohmann::json&)> noFetch =
                [](const std::string&, nlohmann::json&) { return false; };
            MapJiraPresentField("comment", issueFields["comment"], ticket.id, noFetch, ticket);
        }
        if (issueFields.contains("issuetype")) {
            ticket.fieldValues["issuetype"] = NormalizeTrackerFieldValue(issueFields["issuetype"]);
        }
        results.push_back(std::move(ticket));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace jira
} // namespace smatchet
