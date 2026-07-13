#pragma once

#include "ITrackerActivity.h"
#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "ConfigManager.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <mutex>

// SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent backend
// clients; owner=tracker-backend; revisit=2026-12-31)
class PlaneClient : public ITrackerBackend,
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
    PlaneClient();
    ~PlaneClient() override;
    std::string GetTrackerType() const override { return "Plane"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;

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

    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override;

    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;

    void SetMutationCancelToken(std::shared_ptr<std::atomic<bool>> token) override;

    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;

    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;

    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;
    Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    Result<nlohmann::json, TrackerError> BuildUpdatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override;

    Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
    AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths) override;

    TrackerError AddIssueToSprint(const std::string& issueKey, const std::string& sprintId) override;

    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) override;

    // issue-comments PR-C — ITrackerCollaboration comment read + post overrides.
    // FetchIssueComments takes no cfg (resolves on-disk config internally); both
    // resolve the workspace slug + project id + work-item UUID before the HTTP call.
    Result<std::vector<TrackerIssueComment>, TrackerError> FetchIssueComments(const std::string& issueKey) override;

    TrackerError AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                      const std::string& plainText) override;

    std::string ExtractProjectFromQuery(const std::string& query) const override;

    /**
     * GET /api/v1/workspaces/{slug}/projects/ — list projects in the configured workspace.
     * Caches result in-memory for 5 minutes (per-instance). Errors/parse failures
     * log WARN and return an empty vector without poisoning the cache.
     */
    std::vector<RemoteProject> ListProjects() override;

    /** Drop the cached project list so the next ListProjects() refetches. */
    void InvalidateListProjectsCache();

    /**
     * Project-scoped work-item scan + per-issue `/activities/` fetch (PlaneActivityFeed) —
     * one entry per user-authored activity inside the day window.
     */
    Result<std::vector<TrackerActivityEntry>, TrackerError>
    FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                      const std::string& dayTo, const std::string& projectScope,
                      TrackerActivityProgress& progress) override;

    /** Plane has no group concept — Ok-empty (supported, nothing to show). */
    Result<std::vector<std::string>, TrackerError> FetchUserGroupNames(const TrackerConfig& cfg,
                                                                       const std::string& accountId) override;

    /** Plane has no group concept — Ok-empty. */
    Result<std::vector<TrackerUser>, TrackerError> FetchGroupMembers(const TrackerConfig& cfg,
                                                                     const std::string& groupName) override;

    /** Raise the cancel flag for any in-flight FetchUserActivity run. */
    void ClearUserActivity() override;

  private:
    // Per-instance project-list cache (5 min TTL). Mutated under planeCacheMutex_.
    std::vector<RemoteProject> cachedProjects_;
    std::int64_t cachedProjectsAtUnix_ = 0;

    struct CachedState {
        std::string Id;
        std::string Name;
    };
    struct CachedCycle {
        std::string Id;
        std::string Name;
    };
    struct CachedLabel {
        std::string Id;
        std::string Name;
    };
    std::vector<CachedState> cachedStates_;
    std::vector<CachedCycle> cachedCycles_;
    std::vector<TrackerUser> cachedUsers_;
    std::vector<CachedLabel> cachedLabels_;
    std::string planeProjectId_;
    std::string planeProjectIdentifier_;

    std::unordered_map<std::string, std::string> keyToId_;
    mutable std::recursive_mutex planeCacheMutex_;

    // Cancel flag for the in-flight FetchUserActivity run (checked inside the discovery
    // + per-issue activity loops; raised by ClearUserActivity from any thread).
    std::atomic<bool> activityCancel_{false};

    // Shutdown/abort cancel token shared with AppController (set via SetMutationCancelToken). When
    // raised, an in-flight UpdateIssueFields PATCH retry loop aborts promptly so an automation worker
    // blocked in a mutation unwinds at shutdown (mutation-path twin of the search-path cancel, #1529).
    std::shared_ptr<std::atomic<bool>> mutationCancel_;

    static std::unordered_map<std::string, std::string> BuildPlaneHeaders(const TrackerConfig& cfg);

    // Shared body behind FetchIssuesStreamed. `sequenceIdInFilter`, when non-empty, is passed to
    // Plane's work-items list as `sequence_id__in=<csv>` so the server returns only the requested
    // issues (BACKLOG_CODE_REVIEW.md §B4-v2). Empty ≡ the unfiltered full/early-exit sweep; the
    // public FetchIssuesStreamed override always passes empty (a full sync needs every issue).
    TrackerIssueFetchSummary FetchIssuesStreamedImpl(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                     const TrackerConfig* configOverride,
                                                     const ViewsStore* viewsOverride,
                                                     const std::string& sequenceIdInFilter);
};
