// GitHubClient.h — GitHub-backed ITrackerClient.
//
// Scope (T1): skeleton with FetchIssueComments only. All other ITrackerClient
// virtuals return the documented "unsupported on GitHub backend" sentinel.
// Write methods (CommentAdd, LabelAdd, AssigneeSet, StateTransition) land in
// the next slice; AgentProposal context plumbing follows after that.
//
// Build-time gating: the TU is source-list-conditional on SMATCHET_WITH_AGENTIC
// in the root CMakeLists.txt. Callers that hold a `GitHubClient` reference
// must `#if defined(SMATCHET_WITH_AGENTIC)`-gate their include of this header
// so the no-agentic build does not see the symbol.

#ifndef SMATCHET_GITHUB_CLIENT_H
#define SMATCHET_GITHUB_CLIENT_H

#include "ITrackerClient.h"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/**
 * GitHub-backed tracker client. Uses GitHub's REST API
 * (`https://api.github.com` by default) with a Personal Access Token (PAT)
 * via `Authorization: Bearer <pat>`.
 *
 * The PAT is supplied at construction (read by the caller from
 * `cfg.GitHubPat`). The client never logs the PAT verbatim; any error path
 * that includes the bearer header passes the body through
 * `smatchet::ai::pure::RedactProviderErrorBody` first.
 */
class GitHubClient : public ITrackerClient {
  public:
    /**
     * @param baseUrl Defaults to `https://api.github.com`. Pass a different host for
     *                GitHub Enterprise (the REST URL shape is the same — `/api/v3` prefix).
     *                Trailing slash is stripped.
     * @param personalAccessToken PAT used in the `Authorization: Bearer <pat>` header.
     *                            Empty disables the client (every method returns the
     *                            unsupported sentinel until a PAT is configured).
     */
    GitHubClient(std::string baseUrl, std::string personalAccessToken);
    ~GitHubClient() override;

    // ITrackerClient — only FetchIssueComments is real this slice.

    std::string GetTrackerType() const override;
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) override;

    std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                          const ViewsStore* viewsOverride, std::string* outFetchError,
                                          std::string* outWarning) override;

    bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                            const ViewsStore& views, std::vector<CachedTicket>& outTickets,
                            std::string& outError) override;

    bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields,
                           std::string& outError) override;

    bool UpdateField(const std::string& issueId, const TrackerField& field,
                     const std::vector<std::string>& values, std::string& outError) override;

    bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                           nlohmann::json& outPayload, std::string& outError) override;

    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const override;

    /**
     * GET /repos/{owner}/{repo}/issues/{number}/comments — returns the issue
     * comment thread in API-default (oldest-first) order.
     *
     * Errors:
     *   - empty PAT → outError = "GitHub PAT not configured (set cfg.GitHubPat)."
     *   - malformed `issueKey` → outError = parser message from `ParseGitHubIssueKey`
     *   - HTTP failure → outError = redacted single-line summary
     *   - JSON parse failure → outError = "GitHub /comments response is not a JSON array."
     */
    bool FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                            std::string& outError) override;

  private:
    std::string baseUrl_; // e.g. https://api.github.com (no trailing slash).
    std::string pat_;
};

#endif // SMATCHET_GITHUB_CLIENT_H
