#pragma once
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "LocalCacheManager.h" // For CachedTicket struct
#include "TrackerFieldSchema.h"

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
    /**
     * @param outFetchError If non-null, set to a short diagnostic when the sync did not complete
     *        cleanly (e.g. first-page HTTP non-200). Empty on full success. Used for transport vs
     *        hard-failure UX (cached grid).
     */
    virtual std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                                  const JiraConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr,
                                                  std::string* outFetchError = nullptr) = 0;

    // Fetch fields/options for the active tracker. Default impl is unsupported.
    virtual bool FetchFieldCatalog(const JiraConfig& /*cfg*/, TrackerFieldCatalogResult& /*outCatalog*/,
                                   std::string& outError) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }

    // Build a web URL for an issue if the backend supports one.
    virtual std::string BuildBrowseUrl(const JiraConfig& /*cfg*/, const std::string& /*issueKey*/) const { return {}; }

    // Update one or more tracker field values on an issue.
    // `fields` is the backend-specific object payload under the update root.
    virtual bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) = 0;

    /**
     * Create a new issue and return its key (e.g. "PROJ-42") on success.
     * Returns an empty string + sets `outError` on failure.
     * Default impl errors out for backends without create support.
     */
    virtual std::string CreateIssue(const nlohmann::json& /*fields*/, std::string& outError) {
        outError = "CreateIssue is not supported by this backend.";
        return {};
    }

    /**
     * Attach one or more local files to an existing issue.
     * Per-file errors are reported via `outFailures` (path -> message) and do
     * not abort the batch. Default impl errors out for backends without support.
     */
    virtual bool AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/,
                                    std::vector<std::pair<std::string, std::string>>& /*outFailures*/,
                                    std::string& outError) {
        outError = "AttachFilesToIssue is not supported by this backend.";
        return false;
    }

    /**
     * Add an existing issue to a sprint (Jira Agile). Default impl is unsupported.
     */
    virtual bool AddIssueToSprint(const std::string& /*issueKey*/, const std::string& /*sprintId*/,
                                  std::string& outError) {
        outError = "AddIssueToSprint is not supported by this backend.";
        return false;
    }
};
