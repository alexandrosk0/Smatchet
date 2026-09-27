#include "GitHubCommentMappingPure.h"

#include "CommentNodeArrayPure.h"
#include "GitHubClientHelpers.h"

#include <cstdint>

// issue-comments PR-A — pure JSON → TrackerIssueComment mapping. See header.
// Kept cpr-free / SQLite-free so the doctest rig links it without HTTP.

namespace smatchet {
namespace github {

namespace {

// String value of `obj[key]` when present + a string; "" otherwise. Mirrors the
// JsonString helper in GitHubIssueSearchMapping.cpp (kept local so this TU stays
// independent of that one's anonymous namespace).
std::string CommentString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return std::string();
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

// Nested string lookup: `parent[outerKey][innerKey]` when both exist + the leaf
// is a string. "" otherwise.
std::string CommentNestedString(const nlohmann::json& parent, const char* outerKey, const char* innerKey) {
    if (!parent.is_object()) {
        return std::string();
    }
    const auto it = parent.find(outerKey);
    if (it == parent.end() || !it->is_object()) {
        return std::string();
    }
    return CommentString(*it, innerKey);
}

// Stable comment id as a string. GitHub issue-comment ids are int64; render via
// std::to_string. "" when the field is missing / not an integer.
std::string CommentIdString(const nlohmann::json& obj) {
    if (!obj.is_object()) {
        return std::string();
    }
    const auto it = obj.find("id");
    if (it == obj.end() || !it->is_number_integer()) {
        return std::string();
    }
    return std::to_string(it->get<std::int64_t>());
}

} // namespace

std::vector<TrackerIssueComment> MapGitHubIssueComments(const nlohmann::json& commentsArray) {
    return smatchet::tracker::MapCommentNodeArray(commentsArray, [](const nlohmann::json& node) {
        TrackerIssueComment comment;
        comment.Id = CommentIdString(node);
        comment.Author = CommentNestedString(node, "user", "login");
        comment.Body = CommentString(node, "body");
        ParseIso8601CreatedUpdated(CommentString(node, "created_at"), CommentString(node, "updated_at"),
                                   comment.CreatedAtSec, comment.UpdatedAtSec);
        return comment;
    });
}

} // namespace github
} // namespace smatchet
