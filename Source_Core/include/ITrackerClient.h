#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "ITrackerConnectivity.h"
#include "ITrackerIssueReader.h"
#include "ITrackerIssueMutations.h"
#include "TrackerFieldSchema.h"

struct IssueDraft;
struct RequiredFieldSet;

/**
 * One backend comment surfaced to the agentic-flow triage half. Backend-agnostic shape so
 * GitHub issue comments, Jira ADF comments, and Plane comments can land in a single
 * AgentProposal.context bucket. Always opaque to UI — UI never renders rich formatting from
 * `Body`; it's plain text for prompt-builder consumption.
 *
 * `Id` is `std::string` to fit GitHub's int64 issue-comment id and Jira's stringly-typed
 * `1234`-style id without a discriminator field. Time fields are unix epoch seconds because
 * the consumers (poll cursor in `agent_poll_cursor`, prompt-builder sort) need integer
 * comparison, not formatted strings.
 */
struct TrackerIssueComment {
    std::string Id;                // backend-stable comment id
    std::string Author;            // username / handle (GitHub user.login, Jira author.displayName)
    std::string Body;              // raw comment body (markdown for GitHub; plain-text for Jira/Plane)
    std::int64_t CreatedAtSec = 0; // unix epoch seconds
    std::int64_t UpdatedAtSec = 0; // unix epoch seconds (== CreatedAtSec if never edited)
};

class ITrackerClient : public ITrackerIssueReader, public ITrackerConnectivity, public ITrackerIssueMutations {
  public:
    virtual ~ITrackerClient() = default;

    // Fetch fields/options for the active tracker. Default impl is unsupported.
    // PR 6: `projectKey` is the per-operation project for create-meta enrichment (Jira project
    // key, or Plane project UUID). Empty means unscoped — backend returns the global field list.
    // Pre-PR-6 callers used to read this from `cfg.ProjectKey` / `cfg.PlaneProjectId`; those
    // fields are gone and the project is now passed explicitly.
    virtual bool FetchFieldCatalog(const TrackerConfig& /*cfg*/, const std::string& /*projectKey*/,
                                   TrackerFieldCatalogResult& /*outCatalog*/, std::string& outError) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                    std::unordered_map<std::string, bool>& /*outFieldIdCanEdit*/,
                                    std::string& outError) {
        outError = "FetchIssueEditMeta is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueWatchers(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                    std::vector<TrackerUser>& /*outWatchers*/, std::string& outError) {
        outError = "FetchIssueWatchers is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueWatcher(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, std::string& outError) {
        outError = "AddIssueWatcher is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueVotes(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                 std::vector<TrackerUser>& /*outVoters*/, std::string& outError,
                                 int* /*outVoteCount*/ = nullptr, bool* /*outHasVoted*/ = nullptr,
                                 bool* /*outVotersInResponse*/ = nullptr) {
        outError = "FetchIssueVotes is not supported by this backend.";
        return false;
    }

    virtual bool SearchUsersByQuery(const TrackerConfig& /*cfg*/, const std::string& /*query*/,
                                    std::vector<TrackerUser>& /*outUsers*/, std::string& outError) {
        outError = "SearchUsersByQuery is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentPlain(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                      const std::string& /*plainText*/, std::string& outError) {
        outError = "AddIssueCommentPlain is not supported by this backend.";
        return false;
    }

    /**
     * Read all comments on a single issue. Caller-stable ordering — implementations either
     * return newest-first or oldest-first; the agentic-flow prompt-builder sorts on
     * `CreatedAtSec`. Backends without comment support (or with the feature disabled) return
     * `false` and set `outError` to the documented sentinel.
     *
     * `issueKey` shape is backend-specific. GitHub uses `owner/repo#N` (see
     * `GitHubClientHelpers::ParseGitHubIssueKey`). Jira / Plane use their native key forms.
     */
    virtual bool FetchIssueComments(const std::string& /*issueKey*/, std::vector<TrackerIssueComment>& /*outComments*/,
                                    std::string& outError) {
        outError = "FetchIssueComments is not supported by this backend.";
        return false;
    }

    virtual bool AddWorklog(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                            const std::string& /*timeSpent*/, const std::string& /*timeRemaining*/,
                            const std::string& /*adjustEstimate*/, const std::string& /*workDescription*/,
                            const std::string& /*startedDate*/, std::string& outError) {
        outError = "AddWorklog is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentBlameContext(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                             const std::string& /*p4User*/, const std::string& /*functionName*/,
                                             const std::string& /*filePath*/, int /*lineNumber*/,
                                             const std::string& /*changelist*/, const std::string& /*date*/,
                                             bool /*approximated*/, const std::string& /*codeSnippet*/,
                                             std::string& outError) {
        outError = "AddIssueCommentBlameContext is not supported by this backend.";
        return false;
    }

    virtual bool FetchUserGroupNames(const TrackerConfig& /*cfg*/, const std::string& /*accountId*/,
                                     std::vector<std::string>& /*outGroupNames*/, std::string& outError) {
        outError = "FetchUserGroupNames is not supported by this backend.";
        return false;
    }
};
