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
    /** Pretty JSON object from GET /rest/api/3/field for this id (empty if unknown / synthetic). */
    std::string RestFieldDefinitionJson;
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

/** Jira-style duration (e.g. "2h 30m"); empty if seconds <= 0. */
std::string FormatWorkDurationFromSeconds(long long seconds);

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

    /** GET /rest/api/3/issue/{issueKey}/watchers — fills display names / account ids. */
    bool FetchIssueWatchers(const JiraConfig& cfg,
                            const std::string& issueKey,
                            std::vector<JiraUser>& outWatchers,
                            std::string& outError);

    /**
     * GET /rest/api/3/issue/{issueKey}/votes — fills voter users when `voters` is present.
     * On success, optional out-pointers are set from JSON (omit or null to ignore).
     * If `voters` is missing (e.g. permissions), outVoters stays empty and *outVotersArrayInResponse is false.
     */
    bool FetchIssueVotes(const JiraConfig& cfg,
                         const std::string& issueKey,
                         std::vector<JiraUser>& outVoters,
                         std::string& outError,
                         int* outVoteCount = nullptr,
                         bool* outHasVoted = nullptr,
                         bool* outVotersArrayInResponse = nullptr);

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                            const JiraConfig* configOverride = nullptr,
                                            const ViewsStore* viewsOverride = nullptr) override;
};

#endif