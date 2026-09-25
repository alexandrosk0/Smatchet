// Comments-cell tooltip blob formatter doctest — the ONE shared pipeline behind
// fieldValues["comment"] (Jira search mapper via ParseComments, the grid cell's lazy
// first-hover fetch, and the comments-modal post-back). Pure — no HTTP, no ImGui.

#include "Tracker/CommentBlobFormatPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::tracker::ActivityEntryHeader;
using smatchet::tracker::CleanCommentOutputAscii;
using smatchet::tracker::CloseOpenCodeFence;
using smatchet::tracker::EscapeMarkdownInline;
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

TEST_CASE("FormatCommentBlob — single entry renders a bold **Author** YYYY-MM-DD header, blank line, body") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "First comment", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "**Alice** 2024-01-15\n\nFirst comment\n");
}

TEST_CASE("FormatCommentBlob — newest first from unsorted input, thematic break between entries") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Old", "old body", kJan15));
    comments.push_back(MakeComment("New", "new body", kJan15 + 86400));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "**New** 2024-01-16\n\nnew body\n\n---\n\n**Old** 2024-01-15\n\nold body\n");
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
    CHECK(out == "**Alice** 2024-01-15\n\nvisible\n");
}

TEST_CASE("FormatCommentBlob — epoch 0 renders no date without crashing") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "undated", 0));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "**Alice**\n\nundated\n");
}

TEST_CASE("FormatCommentBlob — empty author falls back to Unknown") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("", "body", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out.find("**Unknown**") == 0);
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

TEST_CASE("FormatCommentBlob — Markdown bodies pass through verbatim") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "**bold** and `code`\n\n- item", kJan15));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "**Alice** 2024-01-15\n\n**bold** and `code`\n\n- item\n");
}

TEST_CASE("FormatCommentBlob — an unclosed fence is closed so it cannot swallow the next comment") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Old", "old body", kJan15));
    comments.push_back(MakeComment("New", "```\nunclosed", kJan15 + 86400));
    const std::string out = FormatCommentBlob(comments);
    CHECK(out == "**New** 2024-01-16\n\n```\nunclosed\n```\n\n---\n\n**Old** 2024-01-15\n\nold body\n");
}

TEST_CASE("FormatCommentBlob — author Markdown is escaped in the header") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("a_b*c", "body", kJan15));
    CHECK(FormatCommentBlob(comments).find("**a\\_b\\*c**") == 0);
}

TEST_CASE("EscapeMarkdownInline — escapes Markdown punctuation and folds line breaks") {
    CHECK(EscapeMarkdownInline("plain text 1.2") == "plain text 1.2");
    CHECK(EscapeMarkdownInline("*a* _b_ `c` [d](e) <f> #g |h| ~i~ !j &k $l \\m") ==
          "\\*a\\* \\_b\\_ \\`c\\` \\[d\\](e) \\<f\\> \\#g \\|h\\| \\~i\\~ \\!j \\&k \\$l \\\\m");
    CHECK(EscapeMarkdownInline("a\r\nb\nc") == "a  b c");
    CHECK(EscapeMarkdownInline("") == "");
}

TEST_CASE("CloseOpenCodeFence — balanced input is unchanged") {
    CHECK(CloseOpenCodeFence("") == "");
    CHECK(CloseOpenCodeFence("no fences") == "no fences");
    CHECK(CloseOpenCodeFence("```cpp\nint x;\n```") == "```cpp\nint x;\n```");
    CHECK(CloseOpenCodeFence("~~~\na\n~~~\ntext") == "~~~\na\n~~~\ntext");
}

TEST_CASE("CloseOpenCodeFence — appends the matching fence for an open block") {
    CHECK(CloseOpenCodeFence("```\ncode") == "```\ncode\n```");
    CHECK(CloseOpenCodeFence("```\ncode\n") == "```\ncode\n```");
    CHECK(CloseOpenCodeFence("~~~~\ncode") == "~~~~\ncode\n~~~~");
}

TEST_CASE("CloseOpenCodeFence — only a same-char, long-enough, bare fence closes") {
    // A ~~~ line inside a ``` block, a shorter run, and a run with trailing text do not close it.
    CHECK(CloseOpenCodeFence("````\n~~~\n```\n```` x") == "````\n~~~\n```\n```` x\n````");
    // Two backticks are not a fence; indent of 4+ spaces is not a fence.
    CHECK(CloseOpenCodeFence("``not\n    ```") == "``not\n    ```");
    // Up to 3 spaces of indent still opens.
    CHECK(CloseOpenCodeFence("   ```\nx") == "   ```\nx\n```");
}

TEST_CASE("ActivityEntryHeader — bold escaped author, optional date") {
    CHECK(ActivityEntryHeader("Ann", "2024-01-15") == "**Ann** 2024-01-15");
    CHECK(ActivityEntryHeader("Ann", "") == "**Ann**");
    CHECK(ActivityEntryHeader("", "2024-01-15") == "**Unknown** 2024-01-15");
    CHECK(ActivityEntryHeader("x*y", "") == "**x\\*y**");
}
