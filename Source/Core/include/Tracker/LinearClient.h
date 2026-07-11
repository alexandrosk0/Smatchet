#ifndef SMATCHET_LINEAR_CLIENT_H
#define SMATCHET_LINEAR_CLIENT_H

#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "LinearClientHelpers.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_map>

// LinearClient — fourth tracker backend (docs/plans/linear-tracker-backend.md).
// Linear's API is GraphQL at https://api.linear.app/graphql (POST only); personal
// API keys authenticate via a raw Authorization header (no "Bearer").
// Lifecycle mirrors GitHubClient: factory-owned `unique_ptr<LinearClient>` per
// `Create("linear")`. The ctor snapshots baseUrl + apiKey, but every request
// re-resolves credentials from the live TrackerConfig (issue #979) so a key
// entered/rotated in Preferences takes effect without recreating the client.
// Slice 3 wires the mutation surface: issueUpdate/issueCreate/commentCreate are
// live (LinearIssueMutation.cpp). Collaboration() now self-returns (comment
// posting); Activity() stays nullptr (the activity-feed role is out of MVP scope).

// SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent backend
// clients; owner=tracker-backend; revisit=2026-12-31)
class LinearClient : public ITrackerBackend,
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
    ITrackerCollaboration* Collaboration() override; // comment posting (slice 3)
    ITrackerActivity* Activity() override;           // nullptr — activity feed out of MVP scope
    LinearClient(const std::string& baseUrl, const std::string& apiKey);
    ~LinearClient() override = default;

    // === ITrackerConnectivity ===
    std::string GetTrackerType() const override; // "Linear"
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
    std::string ExtractProjectFromQuery(const std::string& query) const override;
    std::vector<RemoteProject> ListProjects() override; // Linear teams

    // === ITrackerIssueReader ===
    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                          const ViewsStore* viewsOverride, std::string* outFetchError,
                                          std::string* outWarning,
                                          TrackerError* outFetchErrorStructured = nullptr) override;
    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;
    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;

    // === ITrackerFieldCatalog ===
    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override;
    // Linear has no per-issue editmeta/permission API — all fields editable with a
    // write-scoped key. Return an all-true map so AppController caches a positive
    // result and stops re-fetching per UI frame (mirrors GitHubClient).
    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) override;

    // === ITrackerIssueMutations (slice 3: issueUpdate / issueCreate — LinearIssueMutation.cpp) ===
    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;
    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override;

    // === ITrackerCollaboration overrides (slice 3: commentCreate) ===
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    TrackerError AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                      const std::string& plainText) override;

  private:
    /// Issue #979 — resolve apiUrl + apiKey for one request. `configOverride`
    /// non-null → the live cfg key is authoritative (empty = user cleared it);
    /// null → reads the settled on-disk config. Const because read paths call it;
    /// the rotation-visibility log counter is mutable + atomic.
    smatchet::linear::LinearRequestAuth ResolveAuth(const TrackerConfig* configOverride) const;

    std::string baseUrl_; // ctor snapshot fallback for the API URL only (#979)
    std::string apiKey_;  // ctor snapshot, log-visibility only (#979)
    mutable std::atomic<std::size_t> lastLoggedKeyBytes_;
};

#endif // SMATCHET_LINEAR_CLIENT_H
