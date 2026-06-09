#pragma once

#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "ConfigManager.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <mutex>

class PlaneClient : public ITrackerBackend,
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
    PlaneClient();
    ~PlaneClient() override;
    std::string GetTrackerType() const override { return "Plane"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                          const TrackerConfig* configOverride = nullptr,
                                          const ViewsStore* viewsOverride = nullptr,
                                          std::string* outFetchError = nullptr,
                                          std::string* outWarning = nullptr) override;

    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;

    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override;

    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;

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

    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override;

    Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
    AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths) override;

    TrackerError AddIssueToSprint(const std::string& issueKey, const std::string& sprintId) override;

    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) override;

    std::string ExtractProjectFromQuery(const std::string& query) const override;

    /**
     * GET /api/v1/workspaces/{slug}/projects/ — list projects in the configured workspace.
     * Caches result in-memory for 5 minutes (per-instance). Errors/parse failures
     * log WARN and return an empty vector without poisoning the cache.
     */
    std::vector<RemoteProject> ListProjects() override;

    /** Drop the cached project list so the next ListProjects() refetches. */
    void InvalidateListProjectsCache();

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

    static std::unordered_map<std::string, std::string> BuildPlaneHeaders(const TrackerConfig& cfg);
};
