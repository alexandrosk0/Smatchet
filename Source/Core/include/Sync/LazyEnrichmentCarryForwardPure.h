#pragma once

// LazyEnrichmentCarryForwardPure — keeps comment data that a sync row does not carry (Quality
// Pillar 6). Saving a ticket replaces the whole cached row, rich values included. A list sync
// brings no comment bodies for backends whose search payload lacks them (GitHub, Plane) or when
// Jira's follow-up comment fetch fails, so without this every sync would drop the thread the
// comments modal shows offline and the tooltip blob a lazy hover fetch stored. The fields carry
// over only while the comment count is unchanged, so a saved thread never hides a new comment.
// Pure: no I/O, no Logger.h.

#include "CachedTicketTypes.h"

#include <string>

namespace smatchet {
namespace sync {

/// The comment fields of a cached ticket that a later sync row may lack.
struct LazyCommentFields {
    std::string Count;  ///< fieldValues["comments"]
    std::string Blob;   ///< fieldValues["comment"] (tooltip Markdown)
    std::string Thread; ///< fieldRichValues[kCommentThreadRichKey] (structured thread JSON)
};

/// Copies the comment fields out of `ticket`. False (and `out` untouched) when it has neither a
/// blob nor a thread, i.e. nothing worth carrying.
inline bool SnapshotLazyCommentFields(const CachedTicket& ticket, LazyCommentFields& out) {
    const auto threadIt = ticket.fieldRichValues.find(kCommentThreadRichKey);
    const bool hasThread = threadIt != ticket.fieldRichValues.end() && !threadIt->second.empty();
    const std::string& blob = ticket.GetFieldValueRef("comment");
    if (!hasThread && blob.empty()) {
        return false;
    }
    out.Count = ticket.GetFieldValueRef("comments");
    out.Blob = blob;
    out.Thread = hasThread ? threadIt->second : std::string();
    return true;
}

/// Fills the thread and the blob that `incoming` lacks from `prev`, only when both report the same
/// comment count. A value `incoming` already has always wins.
inline void CarryForwardLazyCommentFields(const LazyCommentFields& prev, CachedTicket& incoming) {
    if (incoming.GetFieldValueRef("comments") != prev.Count) {
        return;
    }
    if (!prev.Thread.empty()) {
        const auto threadIt = incoming.fieldRichValues.find(kCommentThreadRichKey);
        if (threadIt == incoming.fieldRichValues.end() || threadIt->second.empty()) {
            incoming.fieldRichValues[kCommentThreadRichKey] = prev.Thread;
        }
    }
    if (!prev.Blob.empty() && incoming.GetFieldValueRef("comment").empty()) {
        incoming.fieldValues["comment"] = prev.Blob;
    }
}

} // namespace sync
} // namespace smatchet
