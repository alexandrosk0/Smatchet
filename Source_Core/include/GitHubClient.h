// GitHubClient.h — GitHub-backed ITrackerClient.
//
// Surface: FetchIssueComments / FetchIssueBody / ListOpenIssuesForRepo (reads)
// plus CommentAdd / LabelAdd / LabelRemove / AssigneeSet / StateTransition
// (writes). The five write methods are concrete on GitHubClient (not yet
// promoted to ITrackerClient virtuals — promoted only when a second
// agentic-write backend appears). All other ITrackerClient virtuals return
// the documented "unsupported on GitHub backend" sentinel.
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
     * GET /repos/{owner}/{repo}/issues/{n} — returns the `title` field from
     * the same payload FetchIssueBody parses. Implemented as a sibling call
     * (separate request) so callers that only need title don't pay for body
     * parsing. Empty title is a valid outcome only when the upstream API
     * elides the field — never expected on a normal issue.
     */
    bool FetchIssueTitle(const std::string& issueKey, std::string& outTitle, std::string& outError);

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

    /**
     * GET https://raw.githubusercontent.com/{owner}/{repo}/HEAD/{path} — raw
     * file content (README.md, CONTRIBUTING.md, AGENTS.md, etc.). 404 returns
     * false + outError = "not found"; other HTTP failures propagate via the
     * shared `ComposeHttpErrorString` redactor. Auth uses the same bearer
     * header as the REST endpoints so private-repo content is accessible.
     * Used by `AgenticContextDoc::GenerateFromGitHubProject` to assemble the
     * per-user project-context document.
     *
     * Read endpoint — no audit-trail.
     */
    bool FetchRawFile(const std::string& owner, const std::string& repo, const std::string& path,
                      std::string& outContent, std::string& outError);

    // ─── Write methods ───────────────────────────────────────────────────────
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

    // ─── H7 PR-thread + workflow surface ─────────────────────────────────────
    //
    // Read methods share `FetchIssueComments` shape (PAT presence check, parse,
    // redacted error compose, no audit-trail). Write methods share the five
    // write methods above (audit Begin/Result, payload size cap, inline bearer).
    //
    // All shapes follow GitHub REST API v2022-11-28. Inline `// API:` comments
    // point at the doc URL so future maintainers can verify wire formats
    // without re-reading this header.

    /**
     * GET /repos/{owner}/{repo}/issues/{number}/comments — PR comments via the
     * issues endpoint (GitHub treats PRs as a superset of issues, so the issue
     * comments endpoint returns the PR's conversation thread; the
     * `/pulls/{number}/comments` variant returns only diff-review comments and
     * is NOT what we want for the H7 watcher). `prKey` uses the canonical
     * `owner/repo#N` shape — same `ParseGitHubIssueKey` parser as the issue
     * variant. Returned in API-default oldest-first order.
     *
     * Errors mirror `FetchIssueComments`.
     */
    bool FetchPrComments(const std::string& prKey, std::vector<TrackerIssueComment>& outComments,
                         std::string& outError);

    /// Request shape for `CreatePullRequest`. `draft` defaults to true — the
    /// agentic flow never opens non-draft PRs (the operator marks ready
    /// manually after audit). `baseBranch` defaults to `"develop"` at the
    /// caller side (this struct keeps it explicit so the call site is self-
    /// documenting).
    struct CreatePullRequestRequest {
        std::string owner;
        std::string repo;
        std::string headBranch;
        std::string baseBranch; // empty -> GitHub rejects; caller defaults
        std::string title;
        std::string body;
        bool draft = true;
    };

    /**
     * POST /repos/{owner}/{repo}/pulls — create a pull request. The response
     * carries the `html_url` of the new PR; `outPrUrl` is populated on 201.
     * Audit action `"CreatePullRequest"`; payload is size-capped via the
     * shared `ShouldRejectAsTooLarge` predicate.
     *
     * API: https://docs.github.com/en/rest/pulls/pulls#create-a-pull-request
     */
    bool CreatePullRequest(const CreatePullRequestRequest& req, std::string& outPrUrl, std::string& outError);

    /// One row of GitHub's check-run list response. Populated only with fields
    /// the agentic loop needs; the full payload carries ~30 fields.
    struct CheckRun {
        std::int64_t id = 0;
        std::string name;       // e.g. "build", "tests"
        std::string status;     // queued | in_progress | completed
        std::string conclusion; // success | failure | neutral | cancelled |
                                // skipped | timed_out | action_required | empty (not yet concluded)
        std::string detailsUrl; // browser-friendly URL of the check run
        std::int64_t startedAtSec = 0;
        std::int64_t completedAtSec = 0;
    };

    /**
     * GET /repos/{owner}/{repo}/commits/{sha}/check-runs — list check runs for
     * a commit. Returns the `check_runs` array of the response (the
     * `total_count` field is discarded; callers infer size from `outRuns`).
     *
     * API: https://docs.github.com/en/rest/checks/runs#list-check-runs-for-a-git-reference
     */
    bool FetchCheckRuns(const std::string& owner, const std::string& repo, const std::string& headSha,
                        std::vector<CheckRun>& outRuns, std::string& outError);

    /// One annotation row from GitHub's check-run annotations endpoint.
    struct CheckRunAnnotation {
        std::string path; // file path relative to repo root
        int startLine = 0;
        int endLine = 0;
        std::string annotationLevel; // notice | warning | failure
        std::string message;
        std::string title; // optional — empty when GitHub omits
    };

    /**
     * GET /repos/{owner}/{repo}/check-runs/{check_run_id}/annotations — list
     * file-level annotations attached to a check run (compiler warnings, lint
     * findings, etc).
     *
     * API: https://docs.github.com/en/rest/checks/runs#list-check-run-annotations
     */
    bool FetchCheckRunAnnotations(const std::string& owner, const std::string& repo, std::int64_t checkRunId,
                                  std::vector<CheckRunAnnotation>& outAnnotations, std::string& outError);

    /**
     * GET /repos/{owner}/{repo}/actions/jobs/{job_id}/logs — fetch the raw log
     * text for a workflow job. GitHub returns a 302 redirect to a presigned
     * blob URL; cpr follows redirects by default. `tailLines > 0` clips the
     * output to the last N lines (newline-delimited) so the result stays
     * bounded; pass 0 for the full body. No audit-trail (read endpoint).
     *
     * API: https://docs.github.com/en/rest/actions/workflow-jobs#download-job-logs-for-a-workflow-run
     */
    bool FetchActionsJobLogs(const std::string& owner, const std::string& repo, std::int64_t jobId, int tailLines,
                             std::string& outLogTail, std::string& outError);

    /**
     * POST /repos/{owner}/{repo}/actions/runs/{run_id}/rerun — re-run all
     * failed jobs in a workflow run. Returns 201 with an empty body on
     * success. Audit action `"RerunWorkflowRun"`. Body is `{}` so size cap
     * never trips.
     *
     * API: https://docs.github.com/en/rest/actions/workflow-runs#re-run-a-workflow
     */
    bool RerunWorkflowRun(const std::string& owner, const std::string& repo, std::int64_t runId, std::string& outError);

    // ─── GraphQL surface (phase 7 — review-thread resolve) ───────────────────
    //
    // GraphQL endpoint POST /graphql. Same auth-header construction as the
    // REST methods (inline bearer, no audit-trail PAT leak). Used by the
    // short-circuit-reject path to mark a CodeRabbit review thread resolved
    // after the reject reply lands — unblocks the merge-gates plan's
    // unresolved-thread gate cleanly. Audit action `"ResolveReviewThread"`.
    //
    // The GraphQL endpoint URL is derived by stripping `/api/v3` from
    // `baseUrl_` (for GitHub Enterprise) and appending `/graphql`; for
    // `https://api.github.com` the endpoint is `https://api.github.com/graphql`.

    /**
     * GraphQL `resolveReviewThread` mutation. `threadId` is the GraphQL
     * node ID of the review thread (NOT the REST comment id — use
     * `LookupReviewThreadIdForComment` to translate a REST comment id when
     * needed).
     *
     * Mutation body:
     *   mutation($threadId: ID!) {
     *     resolveReviewThread(input: {threadId: $threadId}) {
     *       thread { isResolved }
     *     }
     *   }
     *
     * Returns true on 2xx + `data.resolveReviewThread.thread.isResolved == true`.
     * Audit action `"ResolveReviewThread"`.
     */
    bool ResolveReviewThread(const std::string& threadId, std::string& outError);

    /**
     * GraphQL `node(id: ...)` query — translate a REST review-comment id
     * (the numeric id returned by GitHub's `/pulls/{n}/comments` endpoint)
     * into the containing review thread's GraphQL ID. The REST id is
     * base64-encoded into the canonical `PRRC_` node ID shape first
     * (`base64("012:PullRequestReviewComment<id>")`), then queried for
     * `pullRequestReview.thread { id }`.
     *
     * Returns false + outError when the REST id cannot be resolved (deleted
     * comment, mis-encoded id, thread not visible to the PAT). Read endpoint
     * — no audit-trail. Phase 7 picks this lazy-lookup shape over a eager
     * variant on FetchPrComments to keep the diff narrow; rejects are rare.
     */
    bool LookupReviewThreadIdForComment(const std::string& restCommentId, std::string& outThreadId,
                                        std::string& outError);

  private:
    std::string baseUrl_; // e.g. https://api.github.com (no trailing slash).
    std::string pat_;
};

#endif // SMATCHET_GITHUB_CLIENT_H
