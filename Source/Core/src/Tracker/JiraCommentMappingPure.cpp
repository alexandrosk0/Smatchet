#include "JiraCommentMappingPure.h"

#include "CommentNodeArrayPure.h"
#include "GitHubClientHelpers.h"
#include "MarkdownConvert.h"
#include "TrackerFieldValueParser.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

// issue-comments PR-B — pure Jira comment-node → TrackerIssueComment mapping. See
// header. Kept cpr-free / SQLite-free so the doctest rig links it without HTTP.
// Reuses the shared ParseCommentAuthor / AdfBodyToPlainText helpers (declared in
// TrackerFieldValueParser.h) and ParseIso8601ToUnixSec (smatchet::github) — the
// latter strips Jira's millisecond precision before classifying the timezone.

namespace smatchet {
namespace jira {

namespace {

// Real comment ADF nests a few levels (list in list in table cell); far below this. Past it the
// body is server-supplied hostile or broken input, and AdfToMarkdown's recursive walk — whose
// frames are large in sanitizer/Debug builds — could overflow the stack well before its own
// 256-level cap trips.
constexpr std::size_t kMaxConvertibleAdfDepth = 64;

// Iterative (explicit stack, no recursion) so the check itself is stack-safe at any depth.
bool AdfDeeperThan(const nlohmann::json& root, std::size_t maxDepth) {
    std::vector<std::pair<const nlohmann::json*, std::size_t>> pending;
    pending.emplace_back(&root, 0);
    while (!pending.empty()) {
        const nlohmann::json* node = pending.back().first;
        const std::size_t depth = pending.back().second;
        pending.pop_back();
        if (!node->is_structured()) {
            continue;
        }
        if (depth >= maxDepth) {
            return true;
        }
        for (const nlohmann::json& child : *node) {
            pending.emplace_back(&child, depth + 1);
        }
    }
    return false;
}

// ADF → Markdown via the same converter the description tooltip uses, so a comment keeps its
// paragraphs, lists, code and emphasis. A legacy (v2) string body passes through unchanged.
// Falls back to the plain-text flattening when the tree is implausibly deep, the converter
// skipped any node (a panel or expand it cannot represent would otherwise lose its text),
// yields nothing, or throws on a malformed tree — the mapper's never-throws contract.
std::string CommentBodyToMarkdown(const nlohmann::json& body) {
    if (!body.is_object() || AdfDeeperThan(body, kMaxConvertibleAdfDepth)) {
        return AdfBodyToPlainText(body);
    }
    try {
        std::vector<std::string> dropped;
        std::string md = MarkdownConvert::AdfToMarkdown(body, &dropped);
        if (dropped.empty() && !md.empty()) {
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
