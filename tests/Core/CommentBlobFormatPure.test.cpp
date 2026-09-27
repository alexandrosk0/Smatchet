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
using smatchet::tracker::EscapeMarkdownText;
using smatchet::tracker::FormatCommentBlob;
using smatchet::tracker::PlainActivityBlobToMarkdown;
using smatchet::tracker::PreserveLineBreaks;

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

TEST_CASE("FormatCommentBlob — author is trimmed and its restyling characters escaped") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment(" Jane.Doe-2 [bot] a_b*c ", "body", kJan15));
    CHECK(FormatCommentBlob(comments).find("**Jane.Doe-2 \\[bot\\] a\\_b\\*c** 2024-01-15") == 0);
}

TEST_CASE("FormatCommentBlob — single newlines in a body stay line breaks") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Alice", "Thanks,\r\nAlex", kJan15));
    CHECK(FormatCommentBlob(comments) == "**Alice** 2024-01-15\n\nThanks,  \nAlex\n");
}

TEST_CASE("EscapeMarkdownText — escapes every ASCII punctuation char and folds line breaks") {
    CHECK(EscapeMarkdownText("plain text 42") == "plain text 42");
    CHECK(EscapeMarkdownText("*a* _b_ `c` [d](e) <f> #g") == "\\*a\\* \\_b\\_ \\`c\\` \\[d\\]\\(e\\) \\<f\\> \\#g");
    CHECK(EscapeMarkdownText("1. - + = ~ \\") == "1\\. \\- \\+ \\= \\~ \\\\");
    CHECK(EscapeMarkdownText("a\r\nb\nc") == "a  b c");
    CHECK(EscapeMarkdownText("") == "");
    // UTF-8 multi-byte sequences are not ASCII punctuation and pass through untouched.
    CHECK(EscapeMarkdownText("caf\xc3\xa9") == "caf\xc3\xa9");
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
}

TEST_CASE("CloseOpenCodeFence — a backtick run with a backtick after it is inline code, not a fence") {
    CHECK(CloseOpenCodeFence("```git pull --rebase```") == "```git pull --rebase```");
    CHECK(CloseOpenCodeFence("run ```x``` then\n```y` z") == "run ```x``` then\n```y` z");
    // Tilde fences may carry backticks in their info string.
    CHECK(CloseOpenCodeFence("~~~ a`b\ncode") == "~~~ a`b\ncode\n~~~");
}

TEST_CASE("CloseOpenCodeFence — the closer takes the opener's indent (stays inside a list item)") {
    CHECK(CloseOpenCodeFence("1. step\n   ```\n   code") == "1. step\n   ```\n   code\n   ```");
}

TEST_CASE("ActivityEntryHeader — bold author, optional date") {
    CHECK(ActivityEntryHeader("Ann", "2024-01-15") == "**Ann** 2024-01-15");
    CHECK(ActivityEntryHeader("Ann", "") == "**Ann**");
    CHECK(ActivityEntryHeader("", "2024-01-15") == "**Unknown** 2024-01-15");
    CHECK(ActivityEntryHeader("x*y`z<w\\v", "") == "**x\\*y\\`z\\<w\\\\v**");
    CHECK(ActivityEntryHeader("_ops_ ~~x~~ &amp;", "") == "**\\_ops\\_ \\~\\~x\\~\\~ \\&amp;**");
    CHECK(ActivityEntryHeader("two\nlines", "") == "**two lines**");
    CHECK(ActivityEntryHeader("  Jane Doe \t", "") == "**Jane Doe**");
    CHECK(ActivityEntryHeader("   ", "") == "**Unknown**");
}

TEST_CASE("PlainActivityBlobToMarkdown — History entries become the Comments-tooltip Markdown shape") {
    // Exactly what ParseChangelog stores: "[Author] date\nfield: from -> to\n", blank line between.
    const std::string blob = "[Ann] 2024-01-15\nstatus: To Do -> Done\n\n[Ben] 2024-01-16\nsummary: a -> b\n";
    CHECK(PlainActivityBlobToMarkdown(blob) == "**Ann** 2024-01-15\n\nstatus\\: To Do \\-\\> Done\n\n---\n\n"
                                               "**Ben** 2024-01-16\n\nsummary\\: a \\-\\> b\n");
}

TEST_CASE("PlainActivityBlobToMarkdown — values render literally and keep their line breaks") {
    const std::string blob = "[Ann] 2024-01-15\ndescription:  -> # not *a* heading\n- not a list\n\n   indented\n";
    CHECK(PlainActivityBlobToMarkdown(blob) ==
          "**Ann** 2024-01-15\n\ndescription\\:  \\-\\> \\# not \\*a\\* heading\\\n"
          "\\- not a list\n\nindented\n");
}

TEST_CASE("PlainActivityBlobToMarkdown — bracketed value text after a blank line is not an entry header") {
    const std::string blob = "[Ann] 2024-01-15\nsummary: x -> y\n\n[WIP] fix\n";
    CHECK(PlainActivityBlobToMarkdown(blob) == "**Ann** 2024-01-15\n\nsummary\\: x \\-\\> y\n\n\\[WIP\\] fix\n");
}

TEST_CASE("PlainActivityBlobToMarkdown — a header needs an empty or YYYY-MM-DD date") {
    const std::string blob = "[Ann] 2024-01-15\ndescription: a -> x\n\n[1] 2 repro steps\n";
    CHECK(PlainActivityBlobToMarkdown(blob) ==
          "**Ann** 2024-01-15\n\ndescription\\: a \\-\\> x\n\n\\[1\\] 2 repro steps\n");
}

TEST_CASE("PreserveLineBreaks — hard breaks between adjacent text lines only") {
    CHECK(PreserveLineBreaks("") == "");
    CHECK(PreserveLineBreaks("one") == "one");
    CHECK(PreserveLineBreaks("a\nb\nc\n") == "a  \nb  \nc\n");
    CHECK(PreserveLineBreaks("para\n\nnext") == "para\n\nnext");
    CHECK(PreserveLineBreaks("a\r\nb") == "a  \nb");
}

TEST_CASE("PreserveLineBreaks — fenced code is left byte-for-byte (minus CR)") {
    CHECK(PreserveLineBreaks("text\n```\nx\ny\n```\nafter\nmore") == "text  \n```\nx\ny\n```\nafter  \nmore");
    CHECK(PreserveLineBreaks("~~~\nx\ny") == "~~~\nx\ny");
}

TEST_CASE("PlainActivityBlobToMarkdown — empty date, author with brackets, CRLF, empty input") {
    CHECK(PlainActivityBlobToMarkdown("") == "");
    CHECK(PlainActivityBlobToMarkdown("[bot[x]] \r\nlabels: a -> b\r\n") == "**bot\\[x\\]**\n\nlabels\\: a \\-\\> b\n");
    CHECK(PlainActivityBlobToMarkdown("\n\n[... truncated ...]\n") == "\\[\\.\\.\\. truncated \\.\\.\\.\\]\n");
}
