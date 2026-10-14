#pragma once

// SmatchetCommentsModalSeedPure — what the comments modal shows before, or instead of, a live fetch
// (Quality Pillar 6). The structured thread saved with the ticket is complete up to its cap; the
// tooltip blob is the fallback for rows saved before the thread existed and is only a summary (at
// most 20 comments, dates to the day). Parsing is the caller's worker's job, never per frame.

#include "ITrackerCollaboration.h"
#include "Tracker/CommentBlobFormatPure.h"

#include <string>
#include <vector>

namespace SmatchetCommentsModalSeed {

/// Fills `out` from `threadJson` (outPartial = false) or, when that is empty, unreadable or holds no
/// comment, from the display `blob` (outPartial = true). False, with `out` empty and outPartial
/// false, when neither yields a comment.
inline bool PickCommentsSeed(const std::string& threadJson, const std::string& blob,
                             std::vector<TrackerIssueComment>& out, bool& outPartial) {
    outPartial = false;
    if (!threadJson.empty() && smatchet::tracker::ParseCommentThread(threadJson, out) && !out.empty()) {
        return true;
    }
    out = smatchet::tracker::ParseCommentBlob(blob);
    outPartial = !out.empty();
    return outPartial;
}

/// True when the saved structured thread may stand in for a live refetch: it parses, holds the number of
/// comments the ticket's saved count claims (so the saved copy is not a capped or partial one), and was read at
/// the ticket's current `updated` stamp (`threadAt` == `updated`, both known), i.e. nothing changed remotely
/// since. A comment added or edited remotely moves `updated`, so it always fails this and the modal goes live.
inline bool CanServeSavedCommentsWithoutFetch(const std::string& threadJson, const std::string& threadAt,
                                              const std::string& updated, const std::string& savedCount) {
    if (threadJson.empty() || threadAt.empty() || updated.empty() || threadAt != updated || savedCount.empty()) {
        return false;
    }
    std::vector<TrackerIssueComment> comments;
    if (!smatchet::tracker::ParseCommentThread(threadJson, comments) || comments.empty()) {
        return false;
    }
    return savedCount == std::to_string(comments.size());
}

} // namespace SmatchetCommentsModalSeed
