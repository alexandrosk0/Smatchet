#include "PlaneCommentMappingPure.h"

#include "GitHubClientHelpers.h"
#include "TrackerFieldValueParser.h"

#include <cstdint>

// issue-comments PR-C — pure JSON → TrackerIssueComment mapping. See header.
// Kept cpr-free / SQLite-free so the doctest rig links it without HTTP. Reuses
// the GitHub ISO-8601 parser (smatchet::github::ParseIso8601ToUnixSec) so all
// three backends share one timestamp parser.

namespace smatchet {
namespace plane {

namespace {

// Plain-text author for a Plane comment. Plane nests the actor under
// `actor_detail.display_name`; when that object is absent it falls back to the
// flat `created_by` id field. "" when neither is present as a string.
std::string CommentAuthor(const nlohmann::json& obj) {
    if (!obj.is_object()) {
        return std::string();
    }
    const auto it = obj.find("actor_detail");
    if (it != obj.end() && it->is_object()) {
        const std::string display = JsonGetStringIfString(*it, "display_name");
        if (!display.empty()) {
            return display;
        }
    }
    return JsonGetStringIfString(obj, "created_by");
}

} // namespace

std::vector<TrackerIssueComment> MapPlaneIssueComments(const nlohmann::json& nodesArray) {
    std::vector<TrackerIssueComment> out;
    if (!nodesArray.is_array()) {
        return out;
    }
    out.reserve(nodesArray.size());
    for (const auto& node : nodesArray) {
        if (!node.is_object()) {
            continue;
        }
        TrackerIssueComment comment;
        comment.Id = JsonGetStringIfString(node, "id");
        comment.Author = CommentAuthor(node);
        // Plain-text only — `comment_stripped`, never the rich `comment_html`.
        comment.Body = JsonGetStringIfString(node, "comment_stripped");

        const std::string createdIso = JsonGetStringIfString(node, "created_at");
        const Result<std::int64_t, std::string> created = smatchet::github::ParseIso8601ToUnixSec(createdIso);
        if (created) {
            comment.CreatedAtSec = created.value();
        }

        const std::string updatedIso = JsonGetStringIfString(node, "updated_at");
        const Result<std::int64_t, std::string> updated = smatchet::github::ParseIso8601ToUnixSec(updatedIso);
        comment.UpdatedAtSec = updated ? updated.value() : comment.CreatedAtSec;

        out.push_back(std::move(comment));
    }
    return out;
}

} // namespace plane
} // namespace smatchet
