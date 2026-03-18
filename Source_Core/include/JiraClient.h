#ifndef JIRA_CLIENT_H
#define JIRA_CLIENT_H

#include "ITrackerClient.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct JiraFieldOption {
    std::string Id;
    std::string Value;
};

struct JiraField {
    std::string Id;
    std::string Name;
    std::string Type;
    bool ReadOnly = false;
    bool IsArray = false;
    std::string ItemsType;
    bool IsUserType = false;
    bool IsCustom = false;
    std::vector<std::string> AllowedValues;
    std::vector<JiraFieldOption> AllowedValueOptions;
};

struct JiraComponent {
    std::string Id;
    std::string Name;
};

struct JiraUser {
    std::string AccountId;
    std::string DisplayName;
    std::string EmailAddress;
    bool Active = true;
};

class JiraClient : public ITrackerClient {
public:
    bool FetchUsers(const JiraConfig& cfg,
                    std::vector<JiraUser>& outUsers,
                    std::string& outError);

    bool UpdateIssueFields(const std::string& issueId,
                           const nlohmann::json& fields,
                           std::string& outError) override;

    bool FetchFieldCatalog(const JiraConfig& cfg,
                           std::vector<JiraField>& outFields,
                           std::vector<JiraComponent>& outComponents,
                           std::string& outError);

    std::vector<CachedTicket> FetchIssues() override;
};

#endif