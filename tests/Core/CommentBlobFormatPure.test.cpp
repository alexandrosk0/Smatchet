// Comments-cell tooltip blob formatter doctest — the ONE shared pipeline behind
// fieldValues["comment"] (Jira search mapper via ParseComments, the grid cell's lazy
// first-hover fetch, and the comments-modal post-back). Pure — no HTTP, no ImGui.

#include "Tracker/CommentBlobFormatPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::tracker::CleanCommentOutputAscii;
using smatchet::tracker::FormatCommentBlob;

namespace {

TrackerIssueComment MakeComment(const std::string& author, const std::string& body, std::int64_t createdSec) {
    TrackerIssueComment c;
    c.Author = author;
    c.Body = body;
    c.CreatedAtSec = createdSec;
    c.UpdatedAtSec = createdSec;
    return c;
}

// 2024-01-15T00:00:00Z
const std::int64_t kJan15 = 1705276800;

} // namespace

TEST_CASE("FormatCommentBlob — empty input yields empty string") {
    CHECK(FormatCommentBlob(std::vector<TrackerIssueComment>()).empty());
}

TEST_CASE("FormatCommentBlob — single entry renders [Author] YYYY-MM-DD header + body") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "First comment", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "[Alice] 2024-01-15\nFirst comment\n");
}

TEST_CASE("FormatCommentBlob — newest first from unsorted input, blank line between entries") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Old", "old body", kJan15));
    comments.push_back(MakeComment("New", "new body", kJan15 + 86400));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "[New] 2024-01-16\nnew body\n\n[Old] 2024-01-15\nold body\n");
}

TEST_CASE("FormatCommentBlob — tied timestamps keep reverse input order (legacy rbegin parity)") {
    // The original Jira ParseComments iterated the array in reverse; with equal
    // CreatedAtSec the LAST inserted entry must render first.
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("First", "msg-a", kJan15));
    comments.push_back(MakeComment("Second", "msg-b", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out.find("msg-b") < out.find("msg-a"));
}

TEST_CASE("FormatCommentBlob — caps at 20 comments") {
    std::vector<TrackerIssueComment> comments;
    for (int i = 0; i < 50; ++i) {
        comments.push_back(MakeComment("U" + std::to_string(i), "msg-" + std::to_string(i), kJan15 + i));
    }
    const std::string out = FormatCommentBlob(comments);
    // Newest 20 = msg-49 .. msg-30.
    CHECK(out.find("msg-49") != std::string::npos);
    CHECK(out.find("msg-30") != std::string::npos);
    CHECK(out.find("msg-29\n") == std::string::npos);
    CHECK(out.find("msg-0\n") == std::string::npos);
}

TEST_CASE("FormatCommentBlob — caps total length at 12000 chars, truncating at an entry boundary") {
    std::vector<TrackerIssueComment> comments;
    const std::string bigBody(5000, 'x');
    for (int i = 0; i < 5; ++i) {
        comments.push_back(MakeComment("U", bigBody, kJan15 + i));
    }
    const std::string out = FormatCommentBlob(comments);
    CHECK(out.size() <= 12000);
    // Whole entries only: the blob must end exactly where an entry ends.
    CHECK(!out.empty());
    CHECK(out[out.size() - 1] == '\n');
    // Two 5k entries fit; a third would cross 12000.
    CHECK(out.size() > 10000);
}

TEST_CASE("FormatCommentBlob — empty-body entries are skipped and don't consume the cap") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Ghost", "", kJan15 + 100));
    comments.push_back(MakeComment("Alice", "visible", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "[Alice] 2024-01-15\nvisible\n");
}

TEST_CASE("FormatCommentBlob — epoch 0 renders an empty date without crashing") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "undated", 0));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "[Alice] \nundated\n");
}

TEST_CASE("FormatCommentBlob — empty author falls back to Unknown") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("", "body", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out.find("[Unknown]") == 0);
}

TEST_CASE("CleanCommentOutputAscii — rewrites U+2022 bullets to '* ' and passes ASCII through") {
    // The 3-byte bullet becomes "* "; the original following space is preserved.
    CHECK(CleanCommentOutputAscii("\xe2\x80\xa2 item") == "*  item");
    CHECK(CleanCommentOutputAscii("plain") == "plain");
    CHECK(CleanCommentOutputAscii("") == "");
}

TEST_CASE("FormatCommentBlob — bodies pass through CleanCommentOutputAscii") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "\xe2\x80\xa2 bullet", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out.find("* ") != std::string::npos);
    CHECK(out.find("\xe2\x80\xa2") == std::string::npos);
}
