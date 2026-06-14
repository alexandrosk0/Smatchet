#include "JiraCommentMappingPure.h"

#include "GitHubClientHelpers.h"
#include "TrackerFieldValueParser.h"

#include <cstdint>

// issue-comments PR-B — pure Jira comment-node → TrackerIssueComment mapping. See
// header. Kept cpr-free / SQLite-free so the doctest rig links it without HTTP.
// Reuses the shared ParseCommentAuthor / AdfBodyToPlainText helpers (declared in
// TrackerFieldValueParser.h) and ParseIso8601ToUnixSec (smatchet::github) — the
// latter strips Jira's millisecond precision before classifying the timezone.

namespace smatchet {
namespace jira {

std::vector<TrackerIssueComment> MapJiraIssueComments(const nlohmann::json& nodesArray) {
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
        comment.Author = ParseCommentAuthor(node);
        if (node.contains("body")) {
            comment.Body = AdfBodyToPlainText(node["body"]);
        }

        const std::string createdIso = JsonGetStringIfString(node, "created");
        const Result<std::int64_t, std::string> created = smatchet::github::ParseIso8601ToUnixSec(createdIso);
        if (created) {
            comment.CreatedAtSec = created.value();
        }

        const std::string updatedIso = JsonGetStringIfString(node, "updated");
        const Result<std::int64_t, std::string> updated = smatchet::github::ParseIso8601ToUnixSec(updatedIso);
        comment.UpdatedAtSec = updated ? updated.value() : comment.CreatedAtSec;

        out.push_back(std::move(comment));
    }
    return out;
}

} // namespace jira
} // namespace smatchet
