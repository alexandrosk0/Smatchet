#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "SmatchetResult.h"
#include "TrackerError.h"
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

/// Multi-out payload for `FetchIssueVotes` — replaces the old (outVoters, int* outVoteCount,
/// bool* outHasVoted, bool* outVotersInResponse) out-param soup with a single Ok value.
struct TrackerIssueVotes {
    std::vector<TrackerUser> Voters;
    int VoteCount = 0;
    bool HasVoted = false;
    bool VotersArrayInResponse = false;
};

class ITrackerCollaboration {
  public:
    virtual ~ITrackerCollaboration() = default;

    virtual TrackerError AddIssueCommentPlain(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                              const std::string& /*plainText*/) {
        return TrackerErrorInvalidRequest("AddIssueCommentPlain is not supported by this backend.");
    }

    virtual Result<std::vector<TrackerIssueComment>, TrackerError> FetchIssueComments(const std::string& /*issueKey*/) {
        return Result<std::vector<TrackerIssueComment>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchIssueComments is not supported by this backend."));
    }

    virtual Result<std::vector<TrackerUser>, TrackerError> FetchIssueWatchers(const TrackerConfig& /*cfg*/,
                                                                              const std::string& /*issueKey*/) {
        return Result<std::vector<TrackerUser>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchIssueWatchers is not supported by this backend."));
    }

    virtual TrackerError AddIssueWatcher(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) {
        return TrackerErrorInvalidRequest("AddIssueWatcher is not supported by this backend.");
    }

    virtual Result<TrackerIssueVotes, TrackerError> FetchIssueVotes(const TrackerConfig& /*cfg*/,
                                                                    const std::string& /*issueKey*/) {
        return Result<TrackerIssueVotes, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchIssueVotes is not supported by this backend."));
    }

    virtual Result<std::vector<TrackerUser>, TrackerError> SearchUsersByQuery(const TrackerConfig& /*cfg*/,
                                                                              const std::string& /*query*/) {
        return Result<std::vector<TrackerUser>, TrackerError>::Err(
            TrackerErrorInvalidRequest("SearchUsersByQuery is not supported by this backend."));
    }

    virtual TrackerError AddWorklog(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                    const std::string& /*timeSpent*/, const std::string& /*timeRemaining*/,
                                    const std::string& /*adjustEstimate*/, const std::string& /*workDescription*/,
                                    const std::string& /*startedDate*/) {
        return TrackerErrorInvalidRequest("AddWorklog is not supported by this backend.");
    }

    virtual TrackerError AddIssueCommentAnnotateContext(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                                        const std::string& /*p4User*/,
                                                        const std::string& /*functionName*/,
                                                        const std::string& /*filePath*/, int /*lineNumber*/,
                                                        const std::string& /*changelist*/, const std::string& /*date*/,
                                                        bool /*approximated*/, const std::string& /*codeSnippet*/) {
        return TrackerErrorInvalidRequest("AddIssueCommentAnnotateContext is not supported by this backend.");
    }

    virtual Result<std::vector<std::string>, TrackerError> FetchUserGroupNames(const TrackerConfig& /*cfg*/,
                                                                               const std::string& /*accountId*/) {
        return Result<std::vector<std::string>, TrackerError>::Err(
            TrackerErrorInvalidRequest("FetchUserGroupNames is not supported by this backend."));
    }
};
