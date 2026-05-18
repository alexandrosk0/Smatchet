#include "AgenticTriageController.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "GitHubClientHelpers.h"
#include "Logger.h"

#include <utility>

namespace smatchet {
namespace agentic {

namespace {

// Bundle C C-H1 — single OWNER/REPO parser via GitHubClientHelpers. The old
// per-TU `ParseOwnerRepoQuery` here, the loosely-typed slash search in the
// scheduled-poll worker, and the CLI surface had all drifted slightly; the
// shared helper enforces a single rejection contract (empty / no-slash /
// multi-slash / whitespace).
bool ParseOwnerRepoQuery(const std::string& query, std::string& outOwner, std::string& outRepo, std::string& outError) {
    return GitHubClientHelpers::ParseGitHubRepoKey(query, outOwner, outRepo, outError);
}

} // namespace

AgenticTriageController::AgenticTriageController(IGitHubReadClient* github, IInferenceClient* inference,
                                                 AgentProposalStore* store)
    : github_(github), inference_(inference), store_(store) {}

bool AgenticTriageController::TriageIssue(const std::string& issueKey, int& outProposalsInserted,
                                          std::string& outError) {
    outProposalsInserted = 0;
    outError.clear();

    if (github_ == nullptr || inference_ == nullptr || store_ == nullptr) {
        outError = "AgenticTriageController is not wired (github/inference/store nullptr).";
        return false;
    }
    if (issueKey.empty()) {
        outError = "issueKey is empty.";
        return false;
    }

    // Step 1: pull the issue body. GitHub returns `body: null` for issues
    // created without a description; the adapter normalises that to "".
    std::string issueBody;
    std::string fetchErr;
    if (!github_->FetchIssueBody(issueKey, issueBody, fetchErr)) {
        outError = "FetchIssueBody failed: " + fetchErr;
        return false;
    }

    // Step 2: pull comments. An empty thread is valid.
    std::vector<TrackerIssueComment> comments;
    if (!github_->FetchIssueComments(issueKey, comments, fetchErr)) {
        outError = "FetchIssueComments failed: " + fetchErr;
        return false;
    }

    // Step 3: invoke the LLM. The inference client itself enforces the
    // 20-comment / 8 KB body cap; we forward the full vector unchanged.
    std::vector<AgenticInferenceClientPure::ProposalDraft> drafts;
    std::string inferErr;
    if (!inference_->RequestProposals(issueBody, comments, drafts, inferErr)) {
        outError = "RequestProposals failed: " + inferErr;
        return false;
    }

    // Step 4: persist all drafts as Pending rows atomically. Bundle B
    // CR#230:107 — the N drafts the LLM returned for one issue must commit
    // together or none at all. A mid-loop insert failure (e.g. disk full)
    // previously left a fragmentary proposal set visible to the UI; the
    // single InsertMany call wraps every row in one SQLite transaction so
    // the user sees either the complete set or no rows.
    std::vector<AgentProposal> proposals;
    proposals.reserve(drafts.size());
    for (auto& d : drafts) {
        AgentProposal p;
        p.sourceTracker = "github"; // T5 supports the github backend only.
        p.issueKey = issueKey;
        p.action = d.action;
        p.rationale = std::move(d.rationale);
        p.payload = std::move(d.payload);
        // state / createdAtSec / lastUpdatedAtSec are stamped by InsertMany.
        proposals.push_back(std::move(p));
    }

    std::string insertErr;
    if (!store_->InsertMany(proposals, insertErr)) {
        outError = "AgentProposalStore::InsertMany failed: " + insertErr;
        return false;
    }
    outProposalsInserted = static_cast<int>(proposals.size());
    LOG_INFO("AgenticTriageController::TriageIssue %s — inserted %d proposals", issueKey.c_str(),
             outProposalsInserted);
    return true;
}

bool AgenticTriageController::TriageBatch(const std::string& sourceTracker, const std::string& query,
                                          BatchResult& outResult, std::string& outError, int maxIssues) {
    outResult = BatchResult{};
    outError.clear();

    if (sourceTracker != "github") {
        outError = "sourceTracker must be 'github' in T5 (got '" + sourceTracker + "').";
        return false;
    }
    if (github_ == nullptr || inference_ == nullptr || store_ == nullptr) {
        outError = "AgenticTriageController is not wired (github/inference/store nullptr).";
        return false;
    }

    std::string owner;
    std::string repo;
    if (!ParseOwnerRepoQuery(query, owner, repo, outError)) {
        return false;
    }

    std::vector<std::string> keys;
    std::string listErr;
    if (!github_->ListOpenIssuesForRepo(owner, repo, keys, listErr)) {
        outError = "ListOpenIssuesForRepo failed: " + listErr;
        return false;
    }

    // Bundle C CR#230:111 — honour --limit. Discovery returned at most the
    // source-side cap (30 issues per page); --limit further trims the working
    // set so the CLI dry-run never burns more LLM round-trips than the user
    // asked for. maxIssues == 0 disables the extra cap.
    if (maxIssues > 0 && static_cast<int>(keys.size()) > maxIssues) {
        keys.resize(static_cast<std::size_t>(maxIssues));
    }

    outResult.totalIssuesScanned = static_cast<int>(keys.size());
    for (const auto& key : keys) {
        int inserted = 0;
        std::string perErr;
        if (TriageIssue(key, inserted, perErr)) {
            outResult.proposalsInserted += inserted;
        } else {
            // Per-issue failures are non-fatal — record and continue. A
            // transient HTTP error on issue N+1 must not roll back N's
            // proposals (they're already committed to SQLite).
            outResult.perIssueErrors.push_back(std::make_pair(key, perErr));
            LOG_WARN("AgenticTriageController::TriageBatch %s/%s — per-issue %s failed: %s", owner.c_str(),
                     repo.c_str(), key.c_str(), perErr.c_str());
        }
    }
    LOG_INFO("AgenticTriageController::TriageBatch %s/%s — scanned=%d inserted=%d failed=%zu", owner.c_str(),
             repo.c_str(), outResult.totalIssuesScanned, outResult.proposalsInserted,
             outResult.perIssueErrors.size());
    return true;
}

} // namespace agentic
} // namespace smatchet
