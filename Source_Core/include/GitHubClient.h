// GitHubClient.h — GitHub-backed ITrackerClient.
//
// Scope (T1): skeleton with FetchIssueComments only.
// Scope (T2): adds the 5 write methods CommentAdd / LabelAdd / LabelRemove /
//             AssigneeSet / StateTransition. These are concrete on GitHubClient
//             (not yet promoted to ITrackerClient virtuals — promoted only when
//             a second agentic-write backend appears).
// All other ITrackerClient virtuals return the documented "unsupported on
// GitHub backend" sentinel. AgentProposal context plumbing follows in T5.
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

    bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) override;

    bool UpdateField(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
                     std::string& outError) override;

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

    /**
     * GET /repos/{owner}/{repo}/issues/{number} — returns the issue body
     * (markdown) only. The full payload contains many more fields; the agentic
     * triage path needs just the body to seed the LLM prompt. Same auth-header
     * construction as `FetchIssueComments`. This is a READ; per the existing
     * Jira/Plane read-path precedent, no audit-trail entry is emitted (the
     * AppendBegin/AppendResult pair is reserved for write methods).
     *
     * Errors mirror `FetchIssueComments`.
     */
    bool FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError);

    /**
     * GET /repos/{owner}/{repo}/issues?state=open&per_page=30 — returns the
     * first page of open issues in `owner/repo`. Each issue is formatted into
     * Smatchet's canonical `owner/repo#N` key shape and appended to `outKeys`.
     * Capped at 30 issues per call to keep the downstream LLM inference cost
     * predictable.
     *
     * Cursor: when `sinceUnixSec > 0`, the request appends GitHub's `since=<iso>`
     * filter so only issues whose `updated_at` is strictly newer are returned.
     * The scheduled-poll worker passes the previous cursor read from
     * `agent_poll_cursor.last_seen_updated_at`; the first poll passes 0 (no
     * filter, full first page). Bounded API cost + predictable LLM token spend.
     *
     * Errors mirror `FetchIssueComments`. Read endpoint — no audit-trail.
     */
    bool ListOpenIssuesForRepo(const std::string& owner, const std::string& repo, std::vector<std::string>& outKeys,
                               std::string& outError, std::int64_t sinceUnixSec = 0);

    // ─── T2 write methods ────────────────────────────────────────────────────
    //
    // Every method:
    //   1. Parses the issue key via GitHubClientHelpers::ParseGitHubIssueKey.
    //      Fail-early on bad input.
    //   2. Builds the URL via the matching GitHubClientHelpers::BuildIssue*Suffix.
    //   3. Constructs the bearer-auth header INLINE (plan-locked decision #2 in
    //      agentic-flow-implementation.md — do not promote to BuildTrackerHeaders
    //      until a second bearer-auth backend exists). The PAT is never logged.
    //   4. Audits via BackendAuditTrail::AppendBegin/AppendResult with
    //      source="github_client". Both success and failure produce an audit row.
    //   5. Returns true on HTTP 2xx; otherwise outError carries a redacted
    //      one-line cause (status + GitHub's structured "message" if present).

    /**
     * POST /repos/{owner}/{repo}/issues/{number}/comments — add a comment to an
     * issue. Body shape: `{"body":"..."}`. GitHub-side audit-entry source is
     * `"github_client"`, action `"CommentAdd"`.
     */
    bool CommentAdd(const std::string& issueKey, const std::string& body, std::string& outError);

    /**
     * POST /repos/{owner}/{repo}/issues/{number}/labels — add a single label.
     * Body shape: `["label-name"]`. GitHub creates the label on the repo if it
     * doesn't already exist (caller responsibility to keep label vocabulary
     * sane). Audit action `"LabelAdd"`.
     */
    bool LabelAdd(const std::string& issueKey, const std::string& label, std::string& outError);

    /**
     * DELETE /repos/{owner}/{repo}/issues/{number}/labels/{name} — remove a
     * single label from an issue. The label name is URL-encoded in the path
     * (labels can contain `:`, ` `, etc). GitHub returns 200 + the remaining
     * label set when the label was present, 404 when it wasn't on the issue.
     * Audit action `"LabelRemove"`.
     */
    bool LabelRemove(const std::string& issueKey, const std::string& label, std::string& outError);

    /**
     * POST /repos/{owner}/{repo}/issues/{number}/assignees — add a single
     * assignee. Body shape: `{"assignees":["user"]}`. GitHub silently drops
     * users without write permission on the repo; the API still returns 201
     * in that case, so callers cannot rely on success implying assignment.
     * Audit action `"AssigneeSet"`.
     */
    bool AssigneeSet(const std::string& issueKey, const std::string& user, std::string& outError);

    /**
     * PATCH /repos/{owner}/{repo}/issues/{number} — transition issue state to
     * `"open"` or `"closed"`. Any other state string is rejected pre-HTTP via
     * `GitHubClientHelpers::IsValidStateTransitionTarget`. Audit action
     * `"StateTransition"`.
     */
    bool StateTransition(const std::string& issueKey, const std::string& state, std::string& outError);

  private:
    std::string baseUrl_; // e.g. https://api.github.com (no trailing slash).
    std::string pat_;
};

#endif // SMATCHET_GITHUB_CLIENT_H
