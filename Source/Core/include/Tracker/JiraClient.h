#ifndef TRACKER_JIRA_CLIENT_H
#define TRACKER_JIRA_CLIENT_H

#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "ConfigManager.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/** Result of GET /rest/api/3/myself with probe timeouts (periodic connectivity monitor). */
class JiraClient : public ITrackerBackend,
                   public ITrackerIssueReader,
                   public ITrackerConnectivity,
                   public ITrackerFieldCatalog,
                   public ITrackerIssueMutations,
                   public ITrackerCollaboration {
  public:
    ITrackerIssueReader& Reader() override;
    ITrackerConnectivity& Connectivity() override;
    ITrackerFieldCatalog* FieldCatalog() override;
    ITrackerIssueMutations* Mutations() override;
    ITrackerCollaboration* Collaboration() override;
    std::string GetTrackerType() const override { return "Jira"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
    bool FetchUsers(const TrackerConfig& cfg, std::vector<TrackerUser>& outUsers, std::string& outError);

    bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) override;
    bool UpdateField(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
                     std::string& outError) override;
    bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                           nlohmann::json& outPayload, std::string& outError) override;
    bool BuildCreatePayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                            nlohmann::json& outPayload, std::string& outError) override;
    bool BuildUpdatePayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                            nlohmann::json& outPayload, std::string& outError) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

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

    bool FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                           std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                           std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

    bool FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                           TrackerFieldCatalogResult& outCatalog, std::string& outError) override;

    /** GET /rest/api/3/project/{key}/component (paged) — components valid for ONE project.
     * Fills outComponents + outOptions directly (no catalog/fields vector). Used to warm the
     * per-project component map for cross-project grid views. outOptions is sorted by lower-case
     * value then id. */
    bool FetchProjectComponents(const TrackerConfig& cfg, const std::string& projectKey,
                                std::vector<TrackerComponent>& outComponents,
                                std::vector<TrackerFieldOption>& outOptions, std::string& outError) override;

    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;

    /**
     * GET /rest/api/3/issue/{issueKeyOrId}/editmeta
     * Fills outFieldIdCanEdit: field id -> true if Jira lists an edit operation (set/add/remove).
     */
    bool FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId,
                            std::unordered_map<std::string, bool>& outFieldIdCanEdit, std::string& outError) override;

    /** GET /rest/api/3/issue/{issueKey}/watchers — fills display names / account ids. */
    bool FetchIssueWatchers(const TrackerConfig& cfg, const std::string& issueKey,
                            std::vector<TrackerUser>& outWatchers, std::string& outError) override;

    /** POST /rest/api/3/issue/{issueKey}/watchers — adds the authenticated user as a watcher. */
    bool AddIssueWatcher(const TrackerConfig& cfg, const std::string& issueKey, std::string& outError) override;

    /**
     * GET /rest/api/3/issue/{issueKey}/votes — fills voter users when `voters` is present.
     * On success, optional out-pointers are set from JSON (omit or null to ignore).
     * If `voters` is missing (e.g. permissions), outVoters stays empty and *outVotersArrayInResponse is false.
     */
    bool FetchIssueVotes(const TrackerConfig& cfg, const std::string& issueKey, std::vector<TrackerUser>& outVoters,
                         std::string& outError, int* outVoteCount = nullptr, bool* outHasVoted = nullptr,
                         bool* outVotersArrayInResponse = nullptr) override;

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr,
                                          std::string* outWarning = nullptr) override;

    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;

    /**
     * Search by explicit issue keys (same field selection as FetchIssues for `viewStore`).
     * Used to hydrate the local cache when bulk-import rows reference issues outside the current JQL.
     */
    bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                            const ViewsStore& viewStore, std::vector<CachedTicket>& outTickets,
                            std::string& outError) override;

    /** GET /rest/api/3/user/search — for matching Perforce users to Jira accounts. */
    bool SearchUsersByQuery(const TrackerConfig& cfg, const std::string& query, std::vector<TrackerUser>& outUsers,
                            std::string& outError) override;

    /** POST /rest/api/3/issue/{key}/comment with Atlassian Document Format body. */
    bool AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey, const std::string& plainText,
                              std::string& outError) override;

    bool AddWorklog(const TrackerConfig& cfg, const std::string& issueKey, const std::string& timeSpent,
                    const std::string& timeRemaining, const std::string& adjustEstimate,
                    const std::string& workDescription, const std::string& startedDate, std::string& outError) override;

    /** Annotate-context comment: paragraphs plus ADF `codeBlock` for the snippet. */
    bool AddIssueCommentAnnotateContext(const TrackerConfig& cfg, const std::string& issueKey,
                                        const std::string& p4User, const std::string& functionName,
                                        const std::string& filePath, int lineNumber, const std::string& changelist,
                                        const std::string& date, bool approximated, const std::string& codeSnippet,
                                        std::string& outError) override;

    /**
     * Best-effort group names for a user (Cloud may return 403; then outGroupNames stays empty).
     */
    bool FetchUserGroupNames(const TrackerConfig& cfg, const std::string& accountId,
                             std::vector<std::string>& outGroupNames, std::string& outError) override;

    /** Move/add an issue to a sprint via Jira Agile API. */
    bool AddIssueToSprint(const TrackerConfig& cfg, const std::string& issueKey, const std::string& sprintId,
                          std::string& outError);

    std::string ExtractProjectFromQuery(const std::string& query) const override;

    /**
     * GET /rest/api/3/project — list projects visible to the current credentials.
     * Caches result in-memory for 5 minutes (per-instance). Errors/parse failures
     * log WARN and return an empty vector without poisoning the cache.
     * If the API returns >200 entries, only the first 200 are kept.
     */
    std::vector<RemoteProject> ListProjects() override;

    /** Drop the cached project list so the next ListProjects() refetches. */
    void InvalidateListProjectsCache();

    // Per-instance project-list cache (5 min TTL). Mutated under listProjectsMutex_.
  private:
    std::vector<RemoteProject> cachedProjects_;
    std::int64_t cachedProjectsAtUnix_ = 0;
    std::mutex listProjectsMutex_;

    // UpdateIssueFields sub-paths — split to keep the public orchestrator within size caps.
    bool UpdateIssueFieldsViaTransition(const std::string& issueId, const nlohmann::json& statusValue,
                                        const std::string& base, const cpr::Header& headers,
                                        const std::string& auditOp, std::string& outError);
    bool UpdateIssueFieldsViaPut(const std::string& issueId, const nlohmann::json& fieldsAudited,
                                 const std::string& base, const cpr::Header& headers,
                                 const std::string& auditOp, std::string& outError);
};

#endif
