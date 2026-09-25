#include "JiraCommentMappingPure.h"

#include "CommentNodeArrayPure.h"
#include "GitHubClientHelpers.h"
#include "MarkdownConvert.h"
#include "TrackerFieldValueParser.h"

#include <cstdint>
#include <exception>

// issue-comments PR-B — pure Jira comment-node → TrackerIssueComment mapping. See
// header. Kept cpr-free / SQLite-free so the doctest rig links it without HTTP.
// Reuses the shared ParseCommentAuthor / AdfBodyToPlainText helpers (declared in
// TrackerFieldValueParser.h) and ParseIso8601ToUnixSec (smatchet::github) — the
// latter strips Jira's millisecond precision before classifying the timezone.

namespace smatchet {
namespace jira {

namespace {

// ADF → Markdown via the same converter the description tooltip uses, so a comment keeps its
// paragraphs, lists, code and emphasis. A legacy (v2) string body passes through unchanged.
// Falls back to the plain-text flattening if the converter yields nothing (every node dropped)
// or throws on a malformed tree — the mapper's never-throws contract.
std::string CommentBodyToMarkdown(const nlohmann::json& body) {
    if (!body.is_object()) {
        return AdfBodyToPlainText(body);
    }
    try {
        std::string md = MarkdownConvert::AdfToMarkdown(body);
        if (!md.empty()) {
            return md;
        }
    } catch (const std::exception&) {
        // Unconvertible tree — the plain-text flattening below still shows the words.
    }
    return AdfBodyToPlainText(body);
}

} // namespace

std::vector<TrackerIssueComment> MapJiraIssueComments(const nlohmann::json& nodesArray) {
    return smatchet::tracker::MapCommentNodeArray(nodesArray, [](const nlohmann::json& node) {
        TrackerIssueComment comment;
        comment.Id = JsonGetStringIfString(node, "id");
        comment.Author = ParseCommentAuthor(node);
        if (node.contains("body")) {
            comment.Body = CommentBodyToMarkdown(node["body"]);
        }
        smatchet::github::ParseIso8601CreatedUpdated(JsonGetStringIfString(node, "created"),
                                                     JsonGetStringIfString(node, "updated"), comment.CreatedAtSec,
                                                     comment.UpdatedAtSec);
        return comment;
    });
}

} // namespace jira
} // namespace smatchet
