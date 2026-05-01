#pragma once

#include "JiraClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

std::string JsonGetStringIfString(const nlohmann::json& j, const char* key);
std::string JsonValueToCompactString(const nlohmann::json& value);
std::string JsonIdToString(const nlohmann::json& value);
std::string BuildJiraOptionDisplayValue(const nlohmann::json& value);
std::string BuildJiraOptionId(const nlohmann::json& value);
JiraFieldOption JiraFieldOptionFromJson(const nlohmann::json& value);
void MergeJiraFieldOption(std::vector<JiraFieldOption>& target, const JiraFieldOption& incoming);
void RefreshAllowedValuesFromOptions(JiraField& field);
JiraFieldFamily ClassifyJiraFieldFamily(const JiraField& field);
std::string ParseComments(const nlohmann::json& commentsArray);
std::string ParseChangelog(const nlohmann::json& histories);
long long ParseWorkDurationToSeconds(const std::string& input);
std::string FormatJiraTimetrackingDisplay(const nlohmann::json& o);
std::string NormalizeJiraFieldValue(const nlohmann::json& value);
void SortJiraUsersForDisplay(std::vector<JiraUser>& users);
void AppendJiraUsersFromJsonArray(const nlohmann::json& arr, std::vector<JiraUser>& outUsers);
