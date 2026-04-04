#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "LocalCacheManager.h" // For CachedTicket struct

struct JiraConfig;
struct ViewsStore;

class ITrackerClient {
public:
    virtual ~ITrackerClient() = default;
    
    /**
     * Pull issues for the current JQL / tracker query.
     * @param outFullSyncCompleted If non-null, set true only when the backend finished pagination
     *        without an aborted page (safe to drop local rows not present in the returned set).
     * @param configOverride If non-null, use instead of ConfigManager::Load() (JQL, credentials, project).
     * @param viewsOverride If non-null, use instead of reloading views from disk (active view fields).
     */
    virtual std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                                  const JiraConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr) = 0;

    // Update one or more Jira field values on an issue.
    // `fields` is the object payload under Jira's "fields" root.
    virtual bool UpdateIssueFields(const std::string& issueId,
                                   const nlohmann::json& fields,
                                   std::string& outError) = 0;
};