#pragma once
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "LocalCacheManager.h" // For CachedTicket struct
#include "TrackerFieldSchema.h"

struct TrackerConfig;
struct ViewsStore;
struct IssueDraft;
struct RequiredFieldSet;

enum class TrackerReachabilityProbeKind {
    AuthenticatedReachable,
    ReachableAuthOrConfigError,
    TransportDown,
    ServiceUnavailable,
};

struct TrackerReachabilityProbeResult {
    TrackerReachabilityProbeKind Kind = TrackerReachabilityProbeKind::TransportDown;
    std::string Diagnostic;
};

class ITrackerClient {
  public:
    virtual ~ITrackerClient() = default;
    virtual std::string GetTrackerType() const = 0;

    /**
     * Periodic connectivity check.
     */
    virtual TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) = 0;

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
                                                  const TrackerConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr,
                                                  std::string* outFetchError = nullptr) = 0;

    /**
     * Fetch a specific set of issues by their keys.
     */
    virtual bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                    const ViewsStore& views, std::vector<CachedTicket>& outTickets,
                                    std::string& outError) = 0;

    // Fetch fields/options for the active tracker. Default impl is unsupported.
    virtual bool FetchFieldCatalog(const TrackerConfig& /*cfg*/, TrackerFieldCatalogResult& /*outCatalog*/,
                                   std::string& outError) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }

    // Build a web URL for an issue if the backend supports one.
    virtual std::string BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const { return {}; }

    // Update one or more tracker field values on an issue.
    // `fields` is the backend-specific object payload under the update root.
    virtual bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) = 0;

    virtual bool UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values, std::string& outError) = 0;

    virtual bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                                   nlohmann::json& outPayload, std::string& outError) = 0;

    virtual bool BuildCreatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildCreatePayload is not supported by this backend.";
        return false;
    }

    virtual bool BuildUpdatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildUpdatePayload is not supported by this backend.";
        return false;
    }

    virtual std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const = 0;

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

    virtual bool FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                    std::unordered_map<std::string, bool>& /*outFieldIdCanEdit*/, std::string& outError) {
        outError = "FetchIssueEditMeta is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueWatchers(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, std::vector<TrackerUser>& /*outWatchers*/,
                                    std::string& outError) {
        outError = "FetchIssueWatchers is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueVotes(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, std::vector<TrackerUser>& /*outVoters*/,
                                 std::string& outError, int* /*outVoteCount*/ = nullptr, bool* /*outHasVoted*/ = nullptr,
                                 bool* /*outVotersInResponse*/ = nullptr) {
        outError = "FetchIssueVotes is not supported by this backend.";
        return false;
    }

    virtual bool SearchUsersByQuery(const TrackerConfig& /*cfg*/, const std::string& /*query*/, std::vector<TrackerUser>& /*outUsers*/,
                                    std::string& outError) {
        outError = "SearchUsersByQuery is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentPlain(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, const std::string& /*plainText*/,
                                      std::string& outError) {
        outError = "AddIssueCommentPlain is not supported by this backend.";
        return false;
    }

    virtual bool AddWorklog(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                            const std::string& /*timeSpent*/, const std::string& /*timeRemaining*/,
                            const std::string& /*adjustEstimate*/, const std::string& /*workDescription*/,
                            const std::string& /*startedDate*/, std::string& outError) {
        outError = "AddWorklog is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentBlameContext(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, const std::string& /*p4User*/,
                                             const std::string& /*functionName*/, const std::string& /*filePath*/, int /*lineNumber*/,
                                             const std::string& /*changelist*/, const std::string& /*date*/, bool /*approximated*/,
                                             const std::string& /*codeSnippet*/, std::string& outError) {
        outError = "AddIssueCommentBlameContext is not supported by this backend.";
        return false;
    }

    virtual bool FetchUserGroupNames(const TrackerConfig& /*cfg*/, const std::string& /*accountId*/,
                                     std::vector<std::string>& /*outGroupNames*/, std::string& outError) {
        outError = "FetchUserGroupNames is not supported by this backend.";
        return false;
    }
};







