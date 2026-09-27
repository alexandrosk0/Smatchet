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

} // namespace SmatchetCommentsModalSeed
