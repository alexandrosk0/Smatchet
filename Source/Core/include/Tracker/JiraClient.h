#ifndef TRACKER_JIRA_CLIENT_H
#define TRACKER_JIRA_CLIENT_H

#include "ITrackerActivity.h"
#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "ConfigManager.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/** Result of GET /rest/api/3/myself with probe timeouts (periodic connectivity monitor). */
// SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent backend
// clients; owner=tracker-backend; revisit=2026-12-31)
class JiraClient : public ITrackerBackend,
                   public ITrackerIssueReader,
                   public ITrackerConnectivity,
                   public ITrackerFieldCatalog,
                   public ITrackerIssueMutations,
                   public ITrackerCollaboration,
                   public ITrackerActivity {
  public:
    ITrackerIssueReader& Reader() override;
    ITrackerConnectivity& Connectivity() override;
    ITrackerFieldCatalog* FieldCatalog() override;
    ITrackerIssueMutations* Mutations() override;
    ITrackerCollaboration* Collaboration() override;
    ITrackerActivity* Activity() override;
    std::string GetTrackerType() const override { return "Jira"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    bool FetchUsers(const TrackerConfig& cfg, std::vector<TrackerUser>& outUsers, std::string& outError);

    void SetMutationCancelToken(std::shared_ptr<std::atomic<bool>> token) override;
    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;
    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;
    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;
    Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<nlohmann::json, TrackerError> BuildUpdatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

    // POST /rest/api/3/issue with {"fields": <fields>}; Ok = new key (e.g. PROJ-42).
    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override;

    /**
     * Multipart upload to /rest/api/3/issue/{key}/attachments (X-Atlassian-Token: no-check).
     * Per-file failures are collected in the Ok payload and do not abort the remaining uploads.
     * Ok = the per-file failures list (empty when every file uploaded); Err = a hard failure.
     */
    Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
    AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths) override;

    TrackerError AddIssueToSprint(const std::string& issueKey, const std::string& sprintId) override;

    bool FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                           std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                           std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override;

    /** GET /rest/api/3/project/{key}/component (paged) — components valid for ONE project.
     * Returns the components + their dropdown options together (no out-vectors). Used to warm
     * the per-project component map for cross-project grid views. Options are sorted by
     * lower-case value then id. */
    Result<TrackerProjectComponents, TrackerError> FetchProjectComponents(const TrackerConfig& cfg,
                                                                          const std::string& projectKey) override;

    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;

    /**
     * GET /rest/api/3/issue/{issueKeyOrId}/editmeta
     * Fills outFieldIdCanEdit: field id -> true if Jira lists an edit operation (set/add/remove).
     */
    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) override;

    /** GET /rest/api/3/issue/{issueKey}/watchers — Ok = watcher display names / account ids. */
    Result<std::vector<TrackerUser>, TrackerError> FetchIssueWatchers(const TrackerConfig& cfg,
                                                                      const std::string& issueKey) override;

    /** POST /rest/api/3/issue/{issueKey}/watchers — adds the authenticated user as a watcher. */
    TrackerError AddIssueWatcher(const TrackerConfig& cfg, const std::string& issueKey) override;

    /**
     * GET /rest/api/3/issue/{issueKey}/votes — Ok = TrackerIssueVotes (voters + count + has-voted +
     * voters-array-present). If `voters` is missing (e.g. permissions), Voters stays empty and
     * VotersArrayInResponse is false.
     */
    Result<TrackerIssueVotes, TrackerError> FetchIssueVotes(const TrackerConfig& cfg,
                                                            const std::string& issueKey) override;

    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr, std::string* outWarning = nullptr,
                                          TrackerError* outFetchErrorStructured = nullptr) override;

    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;

    /**
     * Search by explicit issue keys (same field selection as FetchIssues for `viewStore`).
     * Used to hydrate the local cache when bulk-import rows reference issues outside the current JQL.
     */
    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& viewStore) override;

    /** GET /rest/api/3/user/search — for matching Perforce users to Jira accounts. */
    Result<std::vector<TrackerUser>, TrackerError> SearchUsersByQuery(const TrackerConfig& cfg,
                                                                      const std::string& query) override;

    /** POST /rest/api/3/issue/{key}/comment with Atlassian Document Format body. */
    TrackerError AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                      const std::string& plainText) override;

    /** GET /rest/api/3/issue/{key}/comment — paginated; ADF body → plain text. */
    Result<std::vector<TrackerIssueComment>, TrackerError> FetchIssueComments(const std::string& issueKey) override;

    TrackerError AddWorklog(const TrackerConfig& cfg, const std::string& issueKey, const std::string& timeSpent,
                            const std::string& timeRemaining, const std::string& adjustEstimate,
                            const std::string& workDescription, const std::string& startedDate) override;

    /** Annotate-context comment: paragraphs plus ADF `codeBlock` for the snippet. */
    TrackerError AddIssueCommentAnnotateContext(const TrackerConfig& cfg, const std::string& issueKey,
                                                const std::string& p4User, const std::string& functionName,
                                                const std::string& filePath, int lineNumber,
                                                const std::string& changelist, const std::string& date,
                                                bool approximated, const std::string& codeSnippet) override;

    /**
     * Best-effort group names for a user (Cloud may return 403; then the Ok list is empty).
     * ITrackerActivity override (relocated from ITrackerCollaboration — one home per capability).
     */
    Result<std::vector<std::string>, TrackerError> FetchUserGroupNames(const TrackerConfig& cfg,
                                                                       const std::string& accountId) override;

    /** GET /rest/api/3/group/member (paged) — members of a named Jira group. */
    Result<std::vector<TrackerUser>, TrackerError> FetchGroupMembers(const TrackerConfig& cfg,
                                                                     const std::string& groupName) override;

    /**
     * JQL `assignee WAS / reporter WAS` discovery + changelog scan (JiraActivityFeed) —
     * one entry per user-authored changelog item inside the day window.
     */
    Result<std::vector<TrackerActivityEntry>, TrackerError>
    FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                      const std::string& dayTo, const std::string& projectScope,
                      TrackerActivityProgress& progress) override;

    /** Raise the cancel flag for any in-flight FetchUserActivity run. */
    void ClearUserActivity() override;

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

    // Cancel flag for the in-flight FetchUserActivity run (checked inside the changelog
    // scan loop; raised by ClearUserActivity from any thread).
    std::atomic<bool> activityCancel_{false};

    // Shutdown/abort cancel token shared with AppController. Set via SetMutationCancelToken; when
    // raised, an in-flight UpdateIssueFields HTTP retry loop aborts promptly so an automation
    // worker blocked in a mutation unwinds at shutdown (mutation-path twin of the search-path
    // cancel plumbing, #1529). A shared_ptr<atomic<bool>> so the token outlives a backend swap.
    std::shared_ptr<std::atomic<bool>> mutationCancel_;

    // UpdateIssueFields sub-paths — split to keep the public orchestrator within size caps.
    bool UpdateIssueFieldsViaTransition(const std::string& issueId, const nlohmann::json& statusValue,
                                        const std::string& base, const cpr::Header& headers, const std::string& auditOp,
                                        std::string& outError, const std::function<bool()>& cancelled);
    bool UpdateIssueFieldsViaPut(const std::string& issueId, const nlohmann::json& fieldsAudited,
                                 const std::string& base, const cpr::Header& headers, const std::string& auditOp,
                                 std::string& outError, const std::function<bool()>& cancelled);
};

#endif
