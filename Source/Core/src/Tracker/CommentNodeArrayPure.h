#ifndef SMATCHET_COMMENT_NODE_ARRAY_PURE_H
#define SMATCHET_COMMENT_NODE_ARRAY_PURE_H

// Shared array walk for the per-backend comment mappers (GitHub / Jira / Plane):
// each supplies only its node → TrackerIssueComment mapping. Header-only, pure.

#include "ITrackerCollaboration.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace smatchet {
namespace tracker {

/// Maps every object element of `nodesArray` through `mapNode`, in order. A non-array
/// argument yields an empty vector and non-object elements are skipped (Pillar 3 —
/// tolerant of a null / partial payload).
template <typename MapNode>
std::vector<TrackerIssueComment> MapCommentNodeArray(const nlohmann::json& nodesArray, MapNode mapNode) {
    std::vector<TrackerIssueComment> out;
    if (!nodesArray.is_array()) {
        return out;
    }
    out.reserve(nodesArray.size());
    for (const nlohmann::json& node : nodesArray) {
        if (node.is_object()) {
            out.push_back(mapNode(node));
        }
    }
    return out;
}

} // namespace tracker
} // namespace smatchet

#endif // SMATCHET_COMMENT_NODE_ARRAY_PURE_H
