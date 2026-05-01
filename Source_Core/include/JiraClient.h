#ifndef JIRA_CLIENT_H
#define JIRA_CLIENT_H

#include "ITrackerClient.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

/**
 * Per-(project, issuetype) metadata collected from Jira's createmeta. Currently
 * tracks required field ids and whether the issuetype is a subtask.
 * Canonical definition (used by AppController / JiraClient only).
 */
struct IssueTypeCreateMeta {
    std::string ProjectKey;
    std::string IssueTypeId;
    std::string IssueTypeName;
    bool IsSubtask = false;
    std::unordered_set<std::string> RequiredFieldIds;
};

/** Result of GET /rest/api/3/myself with probe timeouts (periodic connectivity monitor). */
enum class JiraReachabilityProbeKind {
    AuthenticatedReachable,
    ReachableAuthOrConfigError,
    TransportDown,
    ServiceUnavailable,
};

struct JiraReachabilityProbeResult {
    JiraReachabilityProbeKind Kind = JiraReachabilityProbeKind::TransportDown;
    std::string Diagnostic;
};

class JiraClient : public ITrackerClient {
  public:
    static JiraReachabilityProbeResult ProbeReachability(const JiraConfig& cfg);
    bool FetchUsers(const JiraConfig& cfg, std::vector<JiraUser>& outUsers, std::string& outError);

    bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) override;

    // POST /rest/api/3/issue with {"fields": <fields>}; returns new key (e.g. PROJ-42).
    std::string CreateIssue(const nlohmann::json& fields, std::string& outError) override;

    /**
     * Multipart upload to /rest/api/3/issue/{key}/attachments (X-Atlassian-Token: no-check).
     * Per-file failures are collected in `outFailures` and do not abort the remaining uploads.
     * Returns true when every file uploaded successfully.
     */
    bool AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths,
                            std::vector<std::pair<std::string, std::string>>& outFailures,
                            std::string& outError) override;

    bool AddIssueToSprint(const std::string& issueKey, const std::string& sprintId, std::string& outError) override;

    bool FetchFieldCatalog(const JiraConfig& cfg, std::vector<JiraField>& outFields,
                           std::vector<JiraComponent>& outComponents,
                           std::vector<IssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

    bool FetchFieldCatalog(const JiraConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                           std::string& outError) override;

    std::string BuildBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) const override;

    /**
     * GET /rest/api/3/issue/{issueKeyOrId}/editmeta
     * Fills outFieldIdCanEdit: field id -> true if Jira lists an edit operation (set/add/remove).
     */
    bool FetchIssueEditMeta(const JiraConfig& cfg, const std::string& issueKeyOrId,
                            std::unordered_map<std::string, bool>& outFieldIdCanEdit, std::string& outError);

    /** GET /rest/api/3/issue/{issueKey}/watchers — fills display names / account ids. */
    bool FetchIssueWatchers(const JiraConfig& cfg, const std::string& issueKey, std::vector<JiraUser>& outWatchers,
                            std::string& outError);

    /**
     * GET /rest/api/3/issue/{issueKey}/votes — fills voter users when `voters` is present.
     * On success, optional out-pointers are set from JSON (omit or null to ignore).
     * If `voters` is missing (e.g. permissions), outVoters stays empty and *outVotersArrayInResponse is false.
     */
    bool FetchIssueVotes(const JiraConfig& cfg, const std::string& issueKey, std::vector<JiraUser>& outVoters,
                         std::string& outError, int* outVoteCount = nullptr, bool* outHasVoted = nullptr,
                         bool* outVotersArrayInResponse = nullptr);

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const JiraConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr) override;

    /**
     * Search by explicit issue keys (same field selection as FetchIssues for `viewStore`).
     * Used to hydrate the local cache when bulk-import rows reference issues outside the current JQL.
     */
    bool FetchIssuesForKeys(const JiraConfig& cfg, const std::vector<std::string>& issueKeys,
                            const ViewsStore& viewStore, std::vector<CachedTicket>& outTickets, std::string& outError);

    /** GET /rest/api/3/user/search — for matching Perforce users to Jira accounts. */
    bool SearchUsersByQuery(const JiraConfig& cfg, const std::string& query, std::vector<JiraUser>& outUsers,
                            std::string& outError);

    /** POST /rest/api/3/issue/{key}/comment with Atlassian Document Format body. */
    bool AddIssueCommentPlain(const JiraConfig& cfg, const std::string& issueKey, const std::string& plainText,
                              std::string& outError);

    /** Blame-context comment: paragraphs plus ADF `codeBlock` for the snippet. */
    bool AddIssueCommentBlameContext(const JiraConfig& cfg, const std::string& issueKey, const std::string& p4User,
                                     const std::string& functionName, const std::string& filePath, int lineNumber,
                                     const std::string& changelist, const std::string& date, bool approximated,
                                     const std::string& codeSnippet, std::string& outError);

    /**
     * Best-effort group names for a user (Cloud may return 403; then outGroupNames stays empty).
     */
    bool FetchUserGroupNames(const JiraConfig& cfg, const std::string& accountId,
                             std::vector<std::string>& outGroupNames, std::string& outError);

    /** Move/add an issue to a sprint via Jira Agile API. */
    bool AddIssueToSprint(const JiraConfig& cfg, const std::string& issueKey, const std::string& sprintId,
                          std::string& outError);
};

#endif