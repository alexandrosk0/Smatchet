#pragma once

// Depend on the small POD-types header rather than the heavyweight
// JiraClient.h → ITrackerBackend.h → LocalCacheManager.h (SQLite) cascade.
// Lets the doctest rig under tests/ link this parser without dragging SQLite
// / cpr / ConfigManager into SmatchetTests.
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/** Jira-style duration (e.g. "2h 30m"); empty if seconds <= 0. */
std::string FormatWorkDurationFromSeconds(long long seconds);

std::string JsonGetStringIfString(const nlohmann::json& j, const char* key);
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
