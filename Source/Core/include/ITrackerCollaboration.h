#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "TrackerFieldSchema.h"

struct TrackerConfig;

/**
 * One backend issue comment in a backend-agnostic shape, so GitHub issue comments,
 * Jira ADF comments, and Plane comments can be surfaced through a single type (see
 * `ITrackerCollaboration::FetchIssueComments`). Always opaque to UI — UI never renders
 * rich formatting from `Body`; it's plain text.
 *
 * `Id` is `std::string` to fit GitHub's int64 issue-comment id and Jira's stringly-typed
 * `1234`-style id without a discriminator field. Time fields are unix epoch seconds so
 * consumers can sort by integer comparison rather than parsing formatted strings.
 */
struct TrackerIssueComment {
    std::string Id;                // backend-stable comment id
    std::string Author;            // username / handle (GitHub user.login, Jira author.displayName)
    std::string Body;              // raw comment body (markdown for GitHub; plain-text for Jira/Plane)
    std::int64_t CreatedAtSec = 0; // unix epoch seconds
    std::int64_t UpdatedAtSec = 0; // unix epoch seconds (== CreatedAtSec if never edited)
};

class ITrackerCollaboration {
  public:
    virtual ~ITrackerCollaboration() = default;

    virtual bool AddIssueCommentPlain(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                      const std::string& /*plainText*/, std::string& outError) {
        outError = "AddIssueCommentPlain is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueComments(const std::string& /*issueKey*/, std::vector<TrackerIssueComment>& /*outComments*/,
                                    std::string& outError) {
        outError = "FetchIssueComments is not supported by this backend.";
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

    virtual bool AddWorklog(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                            const std::string& /*timeSpent*/, const std::string& /*timeRemaining*/,
                            const std::string& /*adjustEstimate*/, const std::string& /*workDescription*/,
                            const std::string& /*startedDate*/, std::string& outError) {
        outError = "AddWorklog is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentAnnotateContext(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                                const std::string& /*p4User*/, const std::string& /*functionName*/,
                                                const std::string& /*filePath*/, int /*lineNumber*/,
                                                const std::string& /*changelist*/, const std::string& /*date*/,
                                                bool /*approximated*/, const std::string& /*codeSnippet*/,
                                                std::string& outError) {
        outError = "AddIssueCommentAnnotateContext is not supported by this backend.";
        return false;
    }

    virtual bool FetchUserGroupNames(const TrackerConfig& /*cfg*/, const std::string& /*accountId*/,
                                     std::vector<std::string>& /*outGroupNames*/, std::string& outError) {
        outError = "FetchUserGroupNames is not supported by this backend.";
        return false;
    }
};
