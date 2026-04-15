#ifndef JIRA_CLIENT_H
#define JIRA_CLIENT_H

#include "ITrackerClient.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

enum class JiraFieldFamily {
    Unknown,
    Text,
    Number,
    Date,
    DateTime,
    Labels,
    UserSingle,
    UserMulti,
    SelectSingle,
    SelectMulti,
    CascadingSelect,
    StructuredSingle,
    StructuredMulti,
    Sprint,
    Status,
    IssueType
};

struct JiraFieldOption {
    std::string Id;
    std::string Value;
    std::string SecondaryValue;
    std::string PayloadJson;
    std::vector<JiraFieldOption> Children;
    bool Disabled = false;
};

struct JiraField {
    std::string Id;
    std::string Name;
    std::string Type;
    std::string SchemaSystem;
    std::string SchemaCustom;
    bool ReadOnly = false;
    bool IsArray = false;
    std::string ItemsType;
    bool IsUserType = false;
    bool IsCustom = false;
    JiraFieldFamily Family = JiraFieldFamily::Unknown;
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

    /**
     * GET /rest/api/3/issue/{issueKeyOrId}/editmeta
     * Fills outFieldIdCanEdit: field id -> true if Jira lists an edit operation (set/add/remove).
     */
    bool FetchIssueEditMeta(const JiraConfig& cfg,
                            const std::string& issueKeyOrId,
                            std::unordered_map<std::string, bool>& outFieldIdCanEdit,
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

    /** GET /rest/api/3/user/search — for matching Perforce users to Jira accounts. */
    bool SearchUsersByQuery(const JiraConfig& cfg,
                            const std::string& query,
                            std::vector<JiraUser>& outUsers,
                            std::string& outError);

    /** POST /rest/api/3/issue/{key}/comment with Atlassian Document Format body. */
    bool AddIssueCommentPlain(const JiraConfig& cfg,
                              const std::string& issueKey,
                              const std::string& plainText,
                              std::string& outError);

    /** Blame-context comment: paragraphs plus ADF `codeBlock` for the snippet. */
    bool AddIssueCommentBlameContext(const JiraConfig& cfg,
                                     const std::string& issueKey,
                                     const std::string& p4User,
                                     const std::string& functionName,
                                     const std::string& filePath,
                                     int lineNumber,
                                     const std::string& changelist,
                                     const std::string& date,
                                     bool approximated,
                                     const std::string& codeSnippet,
                                     std::string& outError);

    /**
     * Best-effort group names for a user (Cloud may return 403; then outGroupNames stays empty).
     */
    bool FetchUserGroupNames(const JiraConfig& cfg,
                             const std::string& accountId,
                             std::vector<std::string>& outGroupNames,
                             std::string& outError);

    /** Move/add an issue to a sprint via Jira Agile API. */
    bool AddIssueToSprint(const JiraConfig& cfg,
                          const std::string& issueKey,
                          const std::string& sprintId,
                          std::string& outError);
};

#endif