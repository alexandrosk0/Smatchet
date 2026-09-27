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
using smatchet::tracker::ParseCommentBlob;
using smatchet::tracker::ParseCommentThread;
using smatchet::tracker::PlainActivityBlobToMarkdown;
using smatchet::tracker::PreserveLineBreaks;
using smatchet::tracker::SerializeCommentThread;

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

TEST_CASE("SerializeCommentThread — round trips every field, oldest first") {
    std::vector<TrackerIssueComment> comments;
    TrackerIssueComment newer =
        MakeComment("Bob \"B\" O'Neil", "line one\nline \xc3\xa9 two\n```\ncode\n```", kJan15 + 90);
    newer.Id = "10042";
    newer.UpdatedAtSec = kJan15 + 500;
    TrackerIssueComment older = MakeComment("Ana", "first", kJan15);
    older.Id = "c1";
    comments.push_back(newer);
    comments.push_back(older);

    const std::string json = SerializeCommentThread(comments);
    REQUIRE_FALSE(json.empty());
    std::vector<TrackerIssueComment> parsed;
    REQUIRE(ParseCommentThread(json, parsed));
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].Id == "c1");
    CHECK(parsed[0].Author == "Ana");
    CHECK(parsed[0].Body == "first");
    CHECK(parsed[0].CreatedAtSec == kJan15);
    CHECK(parsed[0].UpdatedAtSec == kJan15);
    CHECK(parsed[1].Id == "10042");
    CHECK(parsed[1].Author == newer.Author);
    CHECK(parsed[1].Body == newer.Body);
    CHECK(parsed[1].CreatedAtSec == kJan15 + 90);
    CHECK(parsed[1].UpdatedAtSec == kJan15 + 500);
}

TEST_CASE("SerializeCommentThread — empty input yields empty string") {
    CHECK(SerializeCommentThread(std::vector<TrackerIssueComment>()).empty());
}

TEST_CASE("SerializeCommentThread — keeps the newest 50 comments") {
    std::vector<TrackerIssueComment> comments;
    for (int i = 0; i < 60; ++i) {
        comments.push_back(MakeComment("U" + std::to_string(i), "msg", kJan15 + i));
    }
    std::vector<TrackerIssueComment> parsed;
    REQUIRE(ParseCommentThread(SerializeCommentThread(comments), parsed));
    REQUIRE(parsed.size() == 50);
    CHECK(parsed.front().Author == "U10");
    CHECK(parsed.back().Author == "U59");
}

TEST_CASE("SerializeCommentThread — stays within 64 KiB by dropping the oldest") {
    std::vector<TrackerIssueComment> comments;
    const std::string body(10 * 1024, 'x');
    for (int i = 0; i < 10; ++i) {
        comments.push_back(MakeComment("U" + std::to_string(i), body, kJan15 + i));
    }
    const std::string json = SerializeCommentThread(comments);
    CHECK(json.size() <= 64u * 1024u);
    std::vector<TrackerIssueComment> parsed;
    REQUIRE(ParseCommentThread(json, parsed));
    REQUIRE(parsed.size() == 6);
    // A contiguous run of the newest comments survives.
    CHECK(parsed.front().Author == "U4");
    CHECK(parsed.back().Author == "U9");
}

TEST_CASE("SerializeCommentThread — a newest comment over the cap on its own leaves nothing") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Small", "fits", kJan15));
    comments.push_back(MakeComment("Huge", std::string(70 * 1024, 'y'), kJan15 + 1));
    CHECK(SerializeCommentThread(comments).empty());
}

TEST_CASE("SerializeCommentThread — invalid UTF-8 is replaced, never thrown") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Ana", std::string("bad \xff\xfe byte"), kJan15));
    std::string json;
    CHECK_NOTHROW(json = SerializeCommentThread(comments));
    std::vector<TrackerIssueComment> parsed;
    REQUIRE(ParseCommentThread(json, parsed));
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].Body.find("bad ") == 0);
}

TEST_CASE("ParseCommentThread — rejects malformed input and tolerates odd entries") {
    std::vector<TrackerIssueComment> out(1);
    CHECK_FALSE(ParseCommentThread("not json", out));
    CHECK(out.empty());
    CHECK_FALSE(ParseCommentThread("{\"id\":1}", out));
    CHECK_FALSE(ParseCommentThread("", out));
    REQUIRE(ParseCommentThread("[1, \"x\", {\"id\": 7, \"author\": 3, \"created\": \"soon\"}, {}]", out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id == "7");
    CHECK(out[0].Author.empty());
    CHECK(out[0].CreatedAtSec == 0);
    CHECK(out[1].Id.empty());
}

TEST_CASE("ParseCommentBlob — inverts FormatCommentBlob: author, day, body and order") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Ana", "first line\nsecond line", kJan15 + 5 * 3600));
    comments.push_back(MakeComment(" Jane_Doe [bot] ", "```\ncode\nmore\n```\nafter\ntext", kJan15 + 2 * 86400));
    comments.push_back(MakeComment("Carl", "kept  \nbreak\n\nnew paragraph", kJan15 + 3 * 86400));
    comments.push_back(MakeComment("", "no author", kJan15 + 4 * 86400));

    const std::vector<TrackerIssueComment> parsed = ParseCommentBlob(FormatCommentBlob(comments));
    REQUIRE(parsed.size() == 4);
    CHECK(parsed[0].Author == "Ana");
    CHECK(parsed[0].CreatedAtSec == kJan15); // the blob keeps the day only
    CHECK(parsed[0].Body == "first line\nsecond line");
    CHECK(parsed[1].Author == "Jane_Doe [bot]");
    CHECK(parsed[1].CreatedAtSec == kJan15 + 2 * 86400);
    CHECK(parsed[1].Body == "```\ncode\nmore\n```\nafter\ntext");
    CHECK(parsed[2].Author == "Carl");
    CHECK(parsed[2].Body == "kept  \nbreak\n\nnew paragraph");
    CHECK(parsed[3].Author == "Unknown");
    CHECK(parsed[3].Body == "no author");
}

TEST_CASE("ParseCommentBlob — a thematic break inside a body does not split the entry") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Ana", "above\n\n---\n\nbelow", kJan15));
    const std::vector<TrackerIssueComment> parsed = ParseCommentBlob(FormatCommentBlob(comments));
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].Body == "above\n\n---\n\nbelow");
}

TEST_CASE("ParseCommentBlob — an undated entry parses with no timestamp") {
    std::vector<TrackerIssueComment> comments;
    comments.push_back(MakeComment("Ana", "undated", 0));
    const std::vector<TrackerIssueComment> parsed = ParseCommentBlob(FormatCommentBlob(comments));
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].CreatedAtSec == 0);
    CHECK(parsed[0].Body == "undated");
}

TEST_CASE("ParseCommentBlob — calendar days convert exactly, leap days and pre-1970 included") {
    const char* dates[] = {"2000-02-29", "2024-02-29", "2100-03-01", "1969-12-31"};
    const std::int64_t want[] = {951782400, 1709164800, 4107542400LL, -86400};
    for (int i = 0; i < 4; ++i) {
        const std::vector<TrackerIssueComment> parsed =
            ParseCommentBlob(std::string("**Ana** ") + dates[i] + "\n\nbody\n");
        REQUIRE(parsed.size() == 1);
        CHECK(parsed[0].CreatedAtSec == want[i]);
    }
}

TEST_CASE("ParseCommentBlob — reads the older plain [Author] YYYY-MM-DD shape") {
    const std::vector<TrackerIssueComment> parsed =
        ParseCommentBlob("[New] 2024-01-16\nnew body\nsecond line\n\n[Old] 2024-01-15\nold body\n");
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].Author == "Old");
    CHECK(parsed[0].CreatedAtSec == kJan15);
    CHECK(parsed[0].Body == "old body");
    CHECK(parsed[1].Author == "New");
    CHECK(parsed[1].Body == "new body\nsecond line");
}

TEST_CASE("ParseCommentBlob — an unrecognised or empty blob yields no entries") {
    CHECK(ParseCommentBlob("").empty());
    CHECK(ParseCommentBlob("just some text\n").empty());
    CHECK(ParseCommentBlob("**unterminated header\n\nbody").empty());
}
