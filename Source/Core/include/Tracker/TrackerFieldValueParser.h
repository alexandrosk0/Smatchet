#pragma once

// Depend on the small POD-types header rather than the heavyweight
// JiraClient.h → ITrackerBackend.h → LocalCacheManager.h (SQLite) cascade.
// Lets the doctest rig under tests/ link this parser without dragging SQLite
// / cpr / ConfigManager into SmatchetTests.
#include "TrackerFieldSchema.h"

// Build-time win: every declaration here takes nlohmann::json by const-reference
// only (no by-value members, no inline bodies), so the forward-decl header is
// enough. TrackerFieldValueParser.cpp includes the full <nlohmann/json.hpp>.
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

/** Jira-style duration (e.g. "2h 30m"); empty if seconds <= 0. */
std::string FormatWorkDurationFromSeconds(long long seconds);

std::string JsonGetStringIfString(const nlohmann::json& j, const char* key);
/** Compact an ISO-8601 date/datetime string to its `YYYY-MM-DD` prefix. Returns `value`
 *  unchanged unless it is at least 10 chars with `-` at indices 4 and 7 — the length guard
 *  makes the fixed-index sniff safe on a short server-supplied value (no out-of-bounds read). */
std::string FormatDateIfIso(const std::string& value);
std::string JsonValueToCompactString(const nlohmann::json& value);
std::string JsonIdToString(const nlohmann::json& value);
std::string BuildTrackerOptionDisplayValue(const nlohmann::json& value);
std::string BuildTrackerOptionId(const nlohmann::json& value);
TrackerFieldOption TrackerFieldOptionFromJson(const nlohmann::json& value);
void MergeTrackerFieldOption(std::vector<TrackerFieldOption>& target, const TrackerFieldOption& incoming);
void RefreshTrackerAllowedValuesFromOptions(TrackerField& field);
TrackerFieldFamily ClassifyTrackerFieldFamily(const TrackerField& field);
std::string ParseComments(const nlohmann::json& commentsArray);
/** Author display name from a Jira comment node (`author.displayName`); "Unknown" if absent. */
std::string ParseCommentAuthor(const nlohmann::json& commentNode);
/** Flatten an ADF (Atlassian Document Format) `body` node — or a plain string body — to plain
 *  text. Object-without-`content` and non-string/non-object inputs return empty. Never throws. */
std::string AdfBodyToPlainText(const nlohmann::json& body);
std::string ParseChangelog(const nlohmann::json& histories);
long long ParseWorkDurationToSeconds(const std::string& input);
std::string FormatTrackerTimetrackingDisplay(const nlohmann::json& o);
std::string NormalizeTrackerFieldValue(const nlohmann::json& value);
void SortTrackerUsersForDisplay(std::vector<TrackerUser>& users);
void AppendTrackerUsersFromJsonArray(const nlohmann::json& arr, std::vector<TrackerUser>& outUsers);
