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
    LOG_TRACE("AgenticTriageController::TriageIssue enter (issueKey=%s)", issueKey.c_str());
    outProposalsInserted = 0;
    outError.clear();

    if (github_ == nullptr || inference_ == nullptr || store_ == nullptr) {
        outError = "AgenticTriageController is not wired (github/inference/store nullptr).";
        LOG_ERROR("AgenticTriageController::TriageIssue exit reason=not_wired (github=%d inference=%d store=%d)",
                  github_ != nullptr ? 1 : 0, inference_ != nullptr ? 1 : 0, store_ != nullptr ? 1 : 0);
        return false;
    }
    if (issueKey.empty()) {
        outError = "issueKey is empty.";
        LOG_TRACE("AgenticTriageController::TriageIssue exit reason=empty_issue_key");
        return false;
    }

    // Step 1: pull the issue body. GitHub returns `body: null` for issues
    // created without a description; the adapter normalises that to "".
    LOG_TRACE("AgenticTriageController::TriageIssue step1 fetching issue body (issueKey=%s)", issueKey.c_str());
    std::string issueBody;
    std::string fetchErr;
    if (!github_->FetchIssueBody(issueKey, issueBody, fetchErr)) {
        outError = "FetchIssueBody failed: " + fetchErr;
        LOG_WARN("AgenticTriageController::TriageIssue FetchIssueBody failed (issueKey=%s): %s", issueKey.c_str(),
                 fetchErr.c_str());
        return false;
    }
    LOG_DEBUG("AgenticTriageController::TriageIssue fetched issue body (issueKey=%s body_bytes=%zu)", issueKey.c_str(),
              issueBody.size());

    // (Schema v3) Also fetch the title — populated into the AgentProposal row
    // so the panel can render upstream context without a separate HTTP call.
    // Failure here is non-fatal: an empty title falls back to the issueKey
    // display in the UI; we LOG_WARN but proceed with triage.
    std::string issueTitle;
    {
        std::string titleErr;
        if (!github_->FetchIssueTitle(issueKey, issueTitle, titleErr)) {
            LOG_WARN("AgenticTriageController::TriageIssue FetchIssueTitle non-fatal failure (issueKey=%s): %s",
                     issueKey.c_str(), titleErr.c_str());
        }
        LOG_DEBUG("AgenticTriageController::TriageIssue fetched issue title (issueKey=%s title_bytes=%zu)",
                  issueKey.c_str(), issueTitle.size());
    }

    // Step 2: pull comments. An empty thread is valid.
    LOG_TRACE("AgenticTriageController::TriageIssue step2 fetching comments (issueKey=%s)", issueKey.c_str());
    std::vector<TrackerIssueComment> comments;
    if (!github_->FetchIssueComments(issueKey, comments, fetchErr)) {
        outError = "FetchIssueComments failed: " + fetchErr;
        LOG_WARN("AgenticTriageController::TriageIssue FetchIssueComments failed (issueKey=%s): %s", issueKey.c_str(),
                 fetchErr.c_str());
        return false;
    }
    LOG_DEBUG("AgenticTriageController::TriageIssue fetched %zu comments (issueKey=%s)", comments.size(),
              issueKey.c_str());

    // Step 3: invoke the LLM. The inference client itself enforces the
    // 20-comment / 8 KB body cap; we forward the full vector unchanged.
    LOG_TRACE("AgenticTriageController::TriageIssue step3 invoking inference (issueKey=%s)", issueKey.c_str());
    std::vector<AgenticInferenceClientPure::ProposalDraft> drafts;
    std::string inferErr;
    if (!inference_->RequestProposals(issueBody, comments, drafts, inferErr)) {
        outError = "RequestProposals failed: " + inferErr;
        LOG_WARN("AgenticTriageController::TriageIssue RequestProposals failed (issueKey=%s): %s", issueKey.c_str(),
                 inferErr.c_str());
        return false;
    }
    LOG_DEBUG("AgenticTriageController::TriageIssue inference returned %zu drafts (issueKey=%s)", drafts.size(),
              issueKey.c_str());

    // Step 4: persist all drafts as Pending rows atomically. Bundle B
    // CR#230:107 — the N drafts the LLM returned for one issue must commit
    // together or none at all. A mid-loop insert failure (e.g. disk full)
    // previously left a fragmentary proposal set visible to the UI; the
    // single InsertMany call wraps every row in one SQLite transaction so
    // the user sees either the complete set or no rows.
    std::vector<AgentProposal> proposals;
    proposals.reserve(drafts.size());
    // (Schema v4) Serialise the comment thread once per issue (shared by every
    // draft from this issue) so the panel can render it without a re-fetch.
    // Empty when the issue has no comments. The shape matches what the LLM
    // user message sees: array of {author, body, createdAtSec}.
    std::string issueCommentsJson;
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : comments) {
            nlohmann::json obj = nlohmann::json::object();
            obj["author"] = c.Author;
            obj["body"] = c.Body;
            obj["createdAtSec"] = static_cast<long long>(c.CreatedAtSec);
            arr.push_back(std::move(obj));
        }
        if (!arr.empty()) {
            issueCommentsJson = arr.dump();
        }
    } catch (const std::exception& ex) {
        LOG_WARN("AgenticTriageController::TriageIssue comments serialisation failed (issueKey=%s): %s",
                 issueKey.c_str(), ex.what());
    }

    for (auto& d : drafts) {
        AgentProposal p;
        p.sourceTracker = "github"; // T5 supports the github backend only.
        p.issueKey = issueKey;
        p.action = d.action;
        p.rationale = std::move(d.rationale);
        p.payload = std::move(d.payload);
        // (Schema v3) Persist upstream context alongside each proposal so the
        // panel renders title + body without re-fetching. Copied (not moved)
        // because multiple drafts for the same issue share the same context.
        p.issueTitle = issueTitle;
        p.issueBody = issueBody;
        p.issueCommentsJson = issueCommentsJson;
        // state / createdAtSec / lastUpdatedAtSec are stamped by InsertMany.
        proposals.push_back(std::move(p));
    }

    LOG_TRACE("AgenticTriageController::TriageIssue step4 persisting %zu drafts (issueKey=%s)", proposals.size(),
              issueKey.c_str());
    std::string insertErr;
    if (!store_->InsertMany(proposals, insertErr)) {
        outError = "AgentProposalStore::InsertMany failed: " + insertErr;
        LOG_ERROR("AgenticTriageController::TriageIssue InsertMany failed (issueKey=%s): %s", issueKey.c_str(),
                  insertErr.c_str());
        return false;
    }
    outProposalsInserted = static_cast<int>(proposals.size());
    LOG_INFO("AgenticTriageController::TriageIssue %s — inserted %d proposals", issueKey.c_str(), outProposalsInserted);
    return true;
}

bool AgenticTriageController::TriageBatch(const std::string& sourceTracker, const std::string& query,
                                          BatchResult& outResult, std::string& outError, int maxIssues) {
    LOG_TRACE("AgenticTriageController::TriageBatch enter (sourceTracker=%s, query=%s, maxIssues=%d)",
              sourceTracker.c_str(), query.c_str(), maxIssues);
    outResult = BatchResult{};
    outError.clear();

    if (sourceTracker != "github") {
        outError = "sourceTracker must be 'github' in T5 (got '" + sourceTracker + "').";
        LOG_WARN("AgenticTriageController::TriageBatch exit reason=bad_source_tracker (got=%s)", sourceTracker.c_str());
        return false;
    }
    if (github_ == nullptr || inference_ == nullptr || store_ == nullptr) {
        outError = "AgenticTriageController is not wired (github/inference/store nullptr).";
        LOG_ERROR("AgenticTriageController::TriageBatch exit reason=not_wired (github=%d inference=%d store=%d)",
                  github_ != nullptr ? 1 : 0, inference_ != nullptr ? 1 : 0, store_ != nullptr ? 1 : 0);
        return false;
    }

    std::string owner;
    std::string repo;
    if (!ParseOwnerRepoQuery(query, owner, repo, outError)) {
        LOG_WARN("AgenticTriageController::TriageBatch ParseOwnerRepoQuery failed (query=%s): %s", query.c_str(),
                 outError.c_str());
        return false;
    }
    LOG_DEBUG("AgenticTriageController::TriageBatch parsed owner=%s repo=%s", owner.c_str(), repo.c_str());

    std::vector<std::string> keys;
    std::string listErr;
    if (!github_->ListOpenIssuesForRepo(owner, repo, keys, listErr)) {
        outError = "ListOpenIssuesForRepo failed: " + listErr;
        LOG_WARN("AgenticTriageController::TriageBatch ListOpenIssuesForRepo failed (owner=%s repo=%s): %s",
                 owner.c_str(), repo.c_str(), listErr.c_str());
        return false;
    }
    LOG_DEBUG("AgenticTriageController::TriageBatch discovered %zu open issues (owner=%s repo=%s)", keys.size(),
              owner.c_str(), repo.c_str());

    // Bundle C CR#230:111 — honour --limit. Discovery returned at most the
    // source-side cap (30 issues per page); --limit further trims the working
    // set so the CLI dry-run never burns more LLM round-trips than the user
    // asked for. maxIssues == 0 disables the extra cap.
    if (maxIssues > 0 && static_cast<int>(keys.size()) > maxIssues) {
        LOG_DEBUG("AgenticTriageController::TriageBatch decision: trim keys from %zu to %d because maxIssues > 0",
                  keys.size(), maxIssues);
        keys.resize(static_cast<std::size_t>(maxIssues));
    }

    outResult.totalIssuesScanned = static_cast<int>(keys.size());
    LOG_INFO("AgenticTriageController::TriageBatch iterating %d issues (owner=%s repo=%s)",
             outResult.totalIssuesScanned, owner.c_str(), repo.c_str());
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
             repo.c_str(), outResult.totalIssuesScanned, outResult.proposalsInserted, outResult.perIssueErrors.size());
    return true;
}

} // namespace agentic
} // namespace smatchet
