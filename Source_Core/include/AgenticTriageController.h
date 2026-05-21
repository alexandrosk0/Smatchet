// AgenticTriageController — the orchestrator class that wires the three layers
// (GitHub read+write, LLM inference, SQLite proposal store) into a single
// end-to-end triage entry point.
//
// Two operations:
//   - TriageIssue(issueKey) — pulls body + comments, invokes inference,
//     persists each draft as a Pending AgentProposal row.
//   - TriageBatch(source, query) — discovers open issues for a repo, then
//     iterates TriageIssue. Bound to 30 issues per call (predictable LLM cost).
//
// Threading: synchronous. The CLI thread is fine; UI callers must wrap in
// `LaunchBackgroundTask` per the pillar-2 contract. The scheduled-poll worker
// is the first UI-thread-reachable caller and brings its Pattern A wrap-up
// with it.
//
// Testability: the controller depends on two small read-only interfaces
// (IGitHubReadClient + IInferenceClient) rather than the concrete
// GitHubClient + AgenticInferenceClient. AgentProposalStore is a leaf and is
// constructed in-memory by the test rig. This keeps the doctest surface free
// of cpr / HTTP — the same testability pattern PR #226 established for the
// GitHubClientHelpers split.
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC in the
// root CMakeLists.txt. The header is consumed only behind
// `#if defined(SMATCHET_WITH_AGENTIC)` guards.

#ifndef SMATCHET_AGENTIC_TRIAGE_CONTROLLER_H
#define SMATCHET_AGENTIC_TRIAGE_CONTROLLER_H

#include "AgenticInferenceClientPure.h" // ProposalDraft
#include "ITrackerClient.h"             // TrackerIssueComment

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class AgentProposalStore;

namespace smatchet {
namespace agentic {

// Minimal read-only seam over the GitHub HTTP client. Extracted so the doctest
// rig can substitute a fake without instantiating cpr. The production
// implementation is in GitHubReadClientAdapter.cpp (a thin wrapper around the
// concrete GitHubClient owned by AppController).
class IGitHubReadClient {
  public:
    virtual ~IGitHubReadClient() = default;

    // GET /repos/{owner}/{repo}/issues/{n} — returns the markdown body field.
    // Empty body is a valid outcome (issue created without description).
    virtual bool FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError) = 0;

    // (Schema v3) Companion to FetchIssueBody — returns the `title` field from
    // the same GET /issues/{n} response. Default impl returns empty title +
    // success so existing fakes (tests + scenario steps) keep compiling
    // without an override. Production GitHubReadAdapter overrides.
    virtual bool FetchIssueTitle(const std::string& /*issueKey*/, std::string& outTitle, std::string& /*outError*/) {
        outTitle.clear();
        return true;
    }

    // GET /repos/{owner}/{repo}/issues/{n}/comments — comment thread, oldest first.
    virtual bool FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                                    std::string& outError) = 0;

    // GET /repos/{owner}/{repo}/issues?state=open — first page only (per_page=30).
    // `sinceUnixSec > 0` appends GitHub's `since=<iso8601>` filter; the scheduled-poll
    // worker passes the previous cursor read from `agent_poll_cursor.last_seen_updated_at`
    // so each poll only fetches issues updated since the last successful run.
    virtual bool ListOpenIssuesForRepo(const std::string& owner, const std::string& repo,
                                       std::vector<std::string>& outKeys, std::string& outError,
                                       std::int64_t sinceUnixSec = 0) = 0;
};

// LLM inference seam. Mirrors `AgenticInferenceClient::RequestProposals` 1:1
// so the production adapter is a one-liner.
class IInferenceClient {
  public:
    virtual ~IInferenceClient() = default;

    virtual bool RequestProposals(const std::string& issueBody, const std::vector<TrackerIssueComment>& comments,
                                  std::vector<AgenticInferenceClientPure::ProposalDraft>& outDrafts,
                                  std::string& outError) = 0;
};

class AgenticTriageController {
  public:
    // Non-owning pointers — AppController owns the three dependencies and
    // outlives this controller per the standard Smatchet lifetime contract.
    AgenticTriageController(IGitHubReadClient* github, IInferenceClient* inference, AgentProposalStore* store);

    // Triage a single issue end-to-end. Sequence:
    //   1. github->FetchIssueBody(issueKey)
    //   2. github->FetchIssueComments(issueKey)
    //   3. inference->RequestProposals(body, comments)
    //   4. For each ProposalDraft → store->Insert as Pending
    //
    // Returns true on success (even if zero proposals survive validation) and
    // writes the inserted count to outProposalsInserted. Returns false on
    // GitHub / inference / DB failure with outError populated. Per-issue
    // failures during batch processing are surfaced via BatchResult below.
    bool TriageIssue(const std::string& issueKey, int& outProposalsInserted, std::string& outError);

    struct BatchResult {
        int totalIssuesScanned = 0;
        int proposalsInserted = 0;
        std::vector<std::pair<std::string, std::string>> perIssueErrors; // (issueKey, error)
    };

    // Triage every open issue in `query` for the named source. Only
    // `sourceTracker == "github"` is supported today; `query` is `OWNER/REPO`. Issue
    // discovery is bounded by the source-side ListOpenIssuesForRepo cap
    // (30 issues). Per-issue triage failures are recorded in
    // outResult.perIssueErrors and do NOT abort the batch — a transient HTTP
    // failure on issue N+1 leaves N's proposals intact.
    //
    // `maxIssues` truncates the discovered issue list to at most that many
    // before iterating (0 = no extra cap beyond what discovery returned).
    // Used by the CLI's `--limit` flag so a dry-run doesn't burn the full
    // discovery cap. Bundle C CR#230:111.
    //
    // Returns true if the discovery call succeeded (regardless of per-issue
    // failure count). Returns false on argument-validation failure or a
    // discovery-level failure (network / auth); outError carries the cause.
    bool TriageBatch(const std::string& sourceTracker, const std::string& query, BatchResult& outResult,
                     std::string& outError, int maxIssues = 0);

  private:
    IGitHubReadClient* github_;
    IInferenceClient* inference_;
    AgentProposalStore* store_;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_AGENTIC_TRIAGE_CONTROLLER_H
