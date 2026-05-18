#include "AgenticTriageController.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "Logger.h"

#include <utility>

namespace smatchet {
namespace agentic {

namespace {

// Parse a `OWNER/REPO` query string into the two parts. Strict: rejects empty
// halves, multiple slashes, embedded whitespace. The slash-count check keeps
// us from accidentally accepting `org/team/repo` which GitHub would 404 on
// anyway, but the early failure surfaces a clearer error to the CLI user.
bool ParseOwnerRepoQuery(const std::string& query, std::string& outOwner, std::string& outRepo, std::string& outError) {
    if (query.empty()) {
        outError = "query is empty (expected OWNER/REPO).";
        return false;
    }
    const auto slash = query.find('/');
    if (slash == std::string::npos) {
        outError = "query must contain '/' separator (got '" + query + "').";
        return false;
    }
    if (query.find('/', slash + 1) != std::string::npos) {
        outError = "query must have exactly one '/' separator (got '" + query + "').";
        return false;
    }
    outOwner = query.substr(0, slash);
    outRepo = query.substr(slash + 1);
    if (outOwner.empty() || outRepo.empty()) {
        outError = "owner and repo must both be non-empty (got '" + query + "').";
        outOwner.clear();
        outRepo.clear();
        return false;
    }
    return true;
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

    // Step 4: persist each draft as a Pending row. A single Insert failure
    // aborts the run — the alternative (insert as many as possible, surface
    // per-row errors) would leave the proposals split across a half-committed
    // state, which the lifecycle invariants in AgentProposal.h forbid.
    for (auto& d : drafts) {
        AgentProposal p;
        p.sourceTracker = "github"; // T5 supports the github backend only.
        p.issueKey = issueKey;
        p.action = d.action;
        p.rationale = std::move(d.rationale);
        p.payload = std::move(d.payload);
        // state / createdAtSec / lastUpdatedAtSec are stamped by Insert.

        std::string insertErr;
        if (!store_->Insert(p, insertErr)) {
            outError = "AgentProposalStore::Insert failed: " + insertErr;
            return false;
        }
        ++outProposalsInserted;
    }
    LOG_INFO("AgenticTriageController::TriageIssue %s — inserted %d proposals", issueKey.c_str(),
             outProposalsInserted);
    return true;
}

bool AgenticTriageController::TriageBatch(const std::string& sourceTracker, const std::string& query,
                                          BatchResult& outResult, std::string& outError) {
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
