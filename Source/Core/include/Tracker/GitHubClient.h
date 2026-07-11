#ifndef SMATCHET_GITHUB_CLIENT_H
#define SMATCHET_GITHUB_CLIENT_H

#include "GitHubClientHelpers.h"
#include "ITrackerActivity.h"
#include "ITrackerBackend.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_map>

// GitHubClient — third tracker backend
// (docs/plans/shipped/github-tracker-backend.md). Tracker-only — does NOT implement
// the PR / check-run / GraphQL surface the deleted agentic flow used.
// Lifecycle: factory-owned `unique_ptr<GitHubClient>` per `Create("github")`
// call (same shape as JiraClient / PlaneClient). Ctor takes a baseUrl + PAT
// snapshot, but every request re-resolves credentials from the live
// TrackerConfig (cfg parameter where present, settled on-disk config on the
// cfg-less mutation paths — JiraClient's pattern) so a PAT entered or rotated
// in Preferences takes effect without recreating the client (issue #979).

// SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent backend
// clients; owner=tracker-backend; revisit=2026-12-31)
class GitHubClient : public ITrackerBackend,
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
    GitHubClient(const std::string& baseUrl, const std::string& pat);
    ~GitHubClient() override = default;

    // === interface overrides ===
    std::string GetTrackerType() const override;
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;
    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                          const ViewsStore* viewsOverride, std::string* outFetchError,
                                          std::string* outWarning,
                                          TrackerError* outFetchErrorStructured = nullptr) override;
    /// Latency fix — overrides the default single-batch
    /// `ITrackerIssueReader::FetchIssuesStreamed` so each GraphQL page is forwarded
    /// to `onBatch` as soon as it returns from GitHub. Without this override the
    /// grid stays empty until all 4 pages complete (~6s wall-clock); with it,
    /// each page lands in ActiveTickets ~1.5s sooner.
    TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch, const CancelCallback& shouldCancel,
                                                 const TrackerConfig* configOverride = nullptr,
                                                 const ViewsStore* viewsOverride = nullptr) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<std::vector<CachedTicket>, TrackerError> FetchIssuesForKeys(const TrackerConfig& cfg,
                                                                       const std::vector<std::string>& issueKeys,
                                                                       const ViewsStore& views) override;
    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override;
    std::string BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const override;
    TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) override;
    TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override;
    Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& draft,
                                                            const std::vector<TrackerField>& catalog) override;
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;
    Result<std::string, TrackerError> CreateIssue(const nlohmann::json& fields) override;
    std::string ExtractProjectFromQuery(const std::string& query) const override;
    std::vector<RemoteProject> ListProjects() override;
    // GitHub issues have no per-issue editmeta concept (no field-level permission API like
    // Jira's `/issue/{key}/editmeta`). All 6 native fields are editable when the PAT has
    // repo write scope. Return an all-`true` map for the static catalog so AppController
    // caches a positive result and stops re-fetching per UI frame.
    Result<std::unordered_map<std::string, bool>, TrackerError>
    FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) override;

    // === ITrackerCollaboration overrides (issue-comments PR-A) ===
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    Result<std::vector<TrackerIssueComment>, TrackerError> FetchIssueComments(const std::string& issueKey) override;
    // SMATCHET_DEVIATION(rule=duplication; reason=interface-mandated override-signature symmetry across independent
    // backend clients; owner=tracker-backend; revisit=2026-12-31)
    TrackerError AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                      const std::string& plainText) override;

    /**
     * Repo issue-events scan (GitHubActivityFeed) — one entry per event authored by
     * `accountId` inside the day window. Scope = cfg Owner/Repo, overridable by an
     * `owner/repo`-shaped projectScope.
     */
    Result<std::vector<TrackerActivityEntry>, TrackerError>
    FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                      const std::string& dayTo, const std::string& projectScope,
                      TrackerActivityProgress& progress) override;

    /**
     * Org-team membership probes against cfg.GitHubOwner (best-effort: a PAT without
     * read:org degrades to Ok-empty, never an error).
     */
    Result<std::vector<std::string>, TrackerError> FetchUserGroupNames(const TrackerConfig& cfg,
                                                                       const std::string& accountId) override;

    /** Members of an org team (matched case-insensitively on name or slug). */
    Result<std::vector<TrackerUser>, TrackerError> FetchGroupMembers(const TrackerConfig& cfg,
                                                                     const std::string& groupName) override;

    /** Raise the cancel flag for any in-flight FetchUserActivity run. */
    void ClearUserActivity() override;

  private:
    /// Issue #979 — resolve baseUrl + PAT for one request. `configOverride` non-null →
    /// the live cfg PAT is used unconditionally (empty = user cleared the credential;
    /// only the base URL falls back to the ctor snapshot); null → reads the settled
    /// on-disk config (mirrors JiraIssueMutation's per-request `ConfigManager::Load()`
    /// pattern on cfg-less call paths). Logs once whenever the resolved PAT byte-count
    /// changes so credential rotation is verifiable in logs alongside the ctor
    /// `pat_bytes=` line. Const because read paths call it; the log-dedup counter is
    /// `mutable` + atomic (called concurrently from the sync worker, the connectivity
    /// probe async, and the UI thread).
    smatchet::github::GitHubRequestAuth ResolveAuth(const TrackerConfig* configOverride) const;

    std::string baseUrl_; // ctor snapshot fallback for the base URL only (issue #979)
    std::string pat_;     // ctor snapshot, log-visibility only; never used for requests (issue #979)
    mutable std::atomic<std::size_t> lastLoggedPatBytes_; // rotation-visibility log dedup (issue #979)

    // Cancel flag for the in-flight FetchUserActivity run — checked inside the
    // page loop, raised by ClearUserActivity from any thread.
    std::atomic<bool> activityCancel_{false};
};

#endif // SMATCHET_GITHUB_CLIENT_H
