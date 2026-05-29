#ifndef SMATCHET_GITHUB_CLIENT_H
#define SMATCHET_GITHUB_CLIENT_H

#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <string>
#include <unordered_map>

// GitHubClient — third tracker backend (PR2 of
// docs/plans/shipped/github-tracker-backend.md). Tracker-only — does NOT implement
// the PR / check-run / GraphQL surface the deleted agentic flow used.
//
// Lifecycle: factory-owned `unique_ptr<GitHubClient>` per `Create("github")`
// call (same shape as JiraClient / PlaneClient). Ctor takes baseUrl + PAT
// snapshot at construction time; runtime PAT rotation requires a fresh
// Create call.

class GitHubClient : public ITrackerBackend,
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
    GitHubClient(const std::string& baseUrl, const std::string& pat);
    ~GitHubClient() override = default;

    // === interface overrides ===
    std::string GetTrackerType() const override;
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                          const ViewsStore* viewsOverride, std::string* outFetchError,
                                          std::string* outWarning) override;
    /// PR12 latency fix — overrides the default single-batch
    /// `ITrackerIssueReader::FetchIssuesStreamed` so each GraphQL page is forwarded
    /// to `onBatch` as soon as it returns from GitHub. Without this override the
    /// grid stays empty until all 4 pages complete (~6s wall-clock); with it,
    /// each page lands in ActiveTickets ~1.5s sooner.
    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;
    bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                            const ViewsStore& views, std::vector<CachedTicket>& outTickets,
                            std::string& outError) override;
    bool FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                           TrackerFieldCatalogResult& outCatalog, std::string& outError) override;
    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;
    bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) override;
    bool UpdateField(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
                     std::string& outError) override;
    bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                           nlohmann::json& outPayload, std::string& outError) override;
    bool BuildCreatePayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                            nlohmann::json& outPayload, std::string& outError) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;
    std::string CreateIssue(const nlohmann::json& fields, std::string& outError) override;
    std::string ExtractProjectFromQuery(const std::string& query) const override;
    std::vector<RemoteProject> ListProjects() override;
    // GitHub issues have no per-issue editmeta concept (no field-level permission API like
    // Jira's `/issue/{key}/editmeta`). All 6 native fields are editable when the PAT has
    // repo write scope. Return an all-`true` map for the static catalog so AppController
    // caches a positive result and stops re-fetching per UI frame.
    bool FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId,
                            std::unordered_map<std::string, bool>& outFieldIdCanEdit, std::string& outError) override;

  private:
    std::string baseUrl_; // e.g. "https://api.github.com" or "https://<enterprise>/api/v3"
    std::string pat_;     // Personal Access Token; empty disables all writes
};

#endif // SMATCHET_GITHUB_CLIENT_H
