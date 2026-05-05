#pragma once

#include "JiraClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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
std::string ParseChangelog(const nlohmann::json& histories);
long long ParseWorkDurationToSeconds(const std::string& input);
std::string FormatTrackerTimetrackingDisplay(const nlohmann::json& o);
std::string NormalizeTrackerFieldValue(const nlohmann::json& value);
void SortTrackerUsersForDisplay(std::vector<TrackerUser>& users);
void AppendTrackerUsersFromJsonArray(const nlohmann::json& arr, std::vector<TrackerUser>& outUsers);






