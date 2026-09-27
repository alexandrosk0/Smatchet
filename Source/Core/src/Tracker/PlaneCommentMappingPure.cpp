#include "PlaneCommentMappingPure.h"

#include "CommentNodeArrayPure.h"
#include "GitHubClientHelpers.h"
#include "MarkdownConvert.h"
#include "TrackerFieldValueParser.h"

#include <cstdint>
#include <exception>

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

// Markdown body: `comment_html` through the same HTML-subset converter the Plane description
// uses. Falls back to the plain `comment_stripped` when there is no HTML, when it holds tags
// outside the converter's allowlist (lossy conversion), when it converts to nothing, or when
// the converter throws — the mapper's never-throws contract.
std::string CommentBodyMarkdown(const nlohmann::json& obj) {
    const std::string html = JsonGetStringIfString(obj, "comment_html");
    if (!html.empty()) {
        try {
            bool fellBack = false;
            std::string md = MarkdownConvert::HtmlSubsetToMarkdown(html, &fellBack);
            while (!md.empty() && (md.back() == '\n' || md.back() == ' ')) {
                md.pop_back();
            }
            if (!fellBack && !md.empty()) {
                return md;
            }
        } catch (const std::exception&) {
            // Unconvertible HTML — the stripped plain text below still shows the words.
        }
    }
    return JsonGetStringIfString(obj, "comment_stripped");
}

} // namespace

std::vector<TrackerIssueComment> MapPlaneIssueComments(const nlohmann::json& nodesArray) {
    return smatchet::tracker::MapCommentNodeArray(nodesArray, [](const nlohmann::json& node) {
        TrackerIssueComment comment;
        comment.Id = JsonGetStringIfString(node, "id");
        comment.Author = CommentAuthor(node);
        comment.Body = CommentBodyMarkdown(node);
        smatchet::github::ParseIso8601CreatedUpdated(JsonGetStringIfString(node, "created_at"),
                                                     JsonGetStringIfString(node, "updated_at"), comment.CreatedAtSec,
                                                     comment.UpdatedAtSec);
        return comment;
    });
}

} // namespace plane
} // namespace smatchet
