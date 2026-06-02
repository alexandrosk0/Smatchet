// Slice 1 of docs/plans/shipped/deterministic-jira-test-backend.md — pure-logic helpers
// extracted from the anonymous namespace of JiraIssueSearch.cpp. No cpr, no Logger,
// no HTTP. See JiraIssueMappingPure.h for the public contract.

#include "JiraIssueMappingPure.h"

#include "ConfigManager.h"
#include "StringUtil.h"
#include "TrackerFieldValueParser.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace jira {

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

// SMATCHET_DEVIATION(rule=function-too-branchy; reason=verbatim extract; owner=unowned; revisit=2026-Q3)
bool AppendCachedTicketFromJiraSearchIssue(
    const nlohmann::json& issue, const std::vector<std::string>& selectedFields,
    const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
    std::vector<CachedTicket>& results) {
    try {
        CachedTicket ticket;
        ticket.id = JsonGetStringIfString(issue, "key");

        nlohmann::json issueFields = issue.value("fields", nlohmann::json::object());
        const auto stringifyForGrid = [&](const nlohmann::json& v) -> std::string {
            if (v.is_object() || v.is_array()) {
                return v.dump();
            }
            return NormalizeTrackerFieldValue(v);
        };

        for (const auto& fieldKey : selectedFields) {
            if (fieldKey == "history") {
                const nlohmann::json changelog = issue.value("changelog", nlohmann::json::object());
                const nlohmann::json histories = changelog.value("histories", nlohmann::json::array());
                ticket.fieldValues["history"] = ParseChangelog(histories);
                continue;
            }
            if (fieldKey == "watchers") {
                if (issueFields.contains("watchers")) {
                    ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watchers"]);
                } else if (issueFields.contains("watches")) {
                    ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watches"]);
                } else {
                    ticket.fieldValues["watchers"] = std::string();
                }
                continue;
            }
            if (issueFields.contains(fieldKey)) {
                const auto& rawValue = issueFields[fieldKey];
                if (fieldKey == "comment" && rawValue.is_object() && rawValue.contains("comments")) {
                    const auto& commentObj = rawValue;
                    const auto& commentsArray = commentObj["comments"];
                    const int totalComments = commentObj.value("total", static_cast<int>(commentsArray.size()));
                    if (commentsArray.is_array() && !commentsArray.empty()) {
                        ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                    } else if (totalComments > 0) {
                        nlohmann::json fetchedComments = nlohmann::json::array();
                        if (fetchIssueComments(ticket.id, fetchedComments) && fetchedComments.is_array()) {
                            ticket.fieldValues[fieldKey] = ParseComments(fetchedComments);
                        } else {
                            ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                        }
                    } else {
                        ticket.fieldValues[fieldKey] = std::string();
                    }
                } else if (fieldKey == "timetracking" || fieldKey == "aggregatetimetracking") {
                    if (rawValue.is_object()) {
                        ticket.fieldValues[fieldKey] = FormatTrackerTimetrackingDisplay(rawValue);
                    } else {
                        ticket.fieldValues[fieldKey] = NormalizeTrackerFieldValue(rawValue);
                    }
                } else if (fieldKey == "attachment" || fieldKey == "attachments") {
                    ticket.fieldValues[fieldKey] = stringifyForGrid(rawValue);
                } else if (fieldKey == "timeoriginalestimate" || fieldKey == "timeestimate" ||
                           fieldKey == "timespent" || fieldKey == "aggregatetimeoriginalestimate" ||
                           fieldKey == "aggregatetimeestimate" || fieldKey == "aggregatetimespent") {
                    long long seconds = 0;
                    if (rawValue.is_number_integer()) {
                        seconds = rawValue.get<long long>();
                    } else if (rawValue.is_number_unsigned()) {
                        seconds = static_cast<long long>(rawValue.get<unsigned long long>());
                    }
                    ticket.fieldValues[fieldKey] = FormatWorkDurationFromSeconds(seconds);
                } else {
                    ticket.fieldValues[fieldKey] = NormalizeTrackerFieldValue(rawValue);
                }
                if (rawValue.is_object() && rawValue.value("type", std::string()) == "doc") {
                    ticket.fieldRichValues[fieldKey] = rawValue.dump();
                }
            } else {
                ticket.fieldValues[fieldKey] = std::string();
            }
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
