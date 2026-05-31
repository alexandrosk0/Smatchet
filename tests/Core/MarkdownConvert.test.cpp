#include <doctest/doctest.h>

#include "MarkdownConvert.h"

#include <string>

using MarkdownConvert::HtmlSubsetToMarkdown;

namespace {

// Convenience: convert and ignore the fell-back flag.
std::string Md(const std::string& html) { return HtmlSubsetToMarkdown(html, nullptr); }

// Convert and capture the fell-back flag.
std::string Md(const std::string& html, bool& fell) { return HtmlSubsetToMarkdown(html, &fell); }

} // namespace

// Each TEST_CASE pins the byte-for-byte output of one tag handler (or a closely
// related family of tags) so the table-driven refactor of HtmlToMarkdown is
// provably behaviour-preserving. Golden strings were captured from the
// pre-refactor if-else implementation (block tags emit a trailing blank line;
// the final newline-collapse pass caps runs at two).

TEST_CASE("HtmlToMarkdown: plain text and entity decoding") {
    CHECK(Md("hello world") == "hello world");
    CHECK(Md("a &amp; b") == "a & b");
    CHECK(Md("&lt;tag&gt;") == "<tag>");
    CHECK(Md("&quot;q&quot; &apos;a&apos;") == "\"q\" 'a'");
    CHECK(Md("nbsp&nbsp;here") == "nbsp here");
    CHECK(Md("dec&#65;") == "decA");
    CHECK(Md("hex&#x41;") == "hexA");
}

TEST_CASE("HtmlToMarkdown: <p> paragraph") {
    CHECK(Md("<p>hello</p>") == "hello\n\n");
    CHECK(Md("<p>one</p><p>two</p>") == "one\n\ntwo\n\n");
}

TEST_CASE("HtmlToMarkdown: <div> block") {
    CHECK(Md("<div>hello</div>") == "hello\n\n");
    CHECK(Md("<div>a</div><div>b</div>") == "a\n\nb\n\n");
}

TEST_CASE("HtmlToMarkdown: headings h1-h6") {
    CHECK(Md("<h1>Title</h1>") == "# Title\n\n");
    CHECK(Md("<h2>Title</h2>") == "## Title\n\n");
    CHECK(Md("<h3>Title</h3>") == "### Title\n\n");
    CHECK(Md("<h4>Title</h4>") == "#### Title\n\n");
    CHECK(Md("<h5>Title</h5>") == "##### Title\n\n");
    CHECK(Md("<h6>Title</h6>") == "###### Title\n\n");
}

TEST_CASE("HtmlToMarkdown: inline emphasis marks") {
    CHECK(Md("<strong>x</strong>") == "**x**");
    CHECK(Md("<b>x</b>") == "**x**");
    CHECK(Md("<em>x</em>") == "*x*");
    CHECK(Md("<i>x</i>") == "*x*");
    CHECK(Md("<s>x</s>") == "~~x~~");
    CHECK(Md("<del>x</del>") == "~~x~~");
    CHECK(Md("<code>x</code>") == "`x`");
    CHECK(Md("<u>x</u>") == "x");
}

TEST_CASE("HtmlToMarkdown: <a> link") {
    CHECK(Md("<a href=\"http://e.com\">link</a>") == "[link](http://e.com)");
    CHECK(Md("<a href=\"\">empty</a>") == "[empty]()");
}

TEST_CASE("HtmlToMarkdown: <img>") {
    CHECK(Md("<img src=\"u.png\" alt=\"alt\">") == "![alt](u.png)");
    CHECK(Md("<img src=\"u.png\">") == "![](u.png)");
    CHECK(Md("<img src=\"u.png\" alt=\"a]b\">") == "![a\\]b](u.png)");
}

TEST_CASE("HtmlToMarkdown: <br>") { CHECK(Md("a<br>b") == "a  \nb"); }

TEST_CASE("HtmlToMarkdown: <hr>") { CHECK(Md("a<hr>b") == "a\n---\n\nb"); }

TEST_CASE("HtmlToMarkdown: <ul> unordered list") { CHECK(Md("<ul><li>a</li><li>b</li></ul>") == "\n- a\n- b\n\n"); }

TEST_CASE("HtmlToMarkdown: <ol> ordered list") { CHECK(Md("<ol><li>a</li><li>b</li></ol>") == "\n1. a\n2. b\n\n"); }

TEST_CASE("HtmlToMarkdown: nested lists") { CHECK(Md("<ul><li>a<ul><li>b</li></ul></li></ul>") == "\n- a\n  - b\n\n"); }

TEST_CASE("HtmlToMarkdown: <blockquote>") { CHECK(Md("<blockquote>q</blockquote>") == "> q\n\n"); }

TEST_CASE("HtmlToMarkdown: <pre> code fence") { CHECK(Md("<pre>code</pre>") == "\n```\ncode\n```\n\n"); }

TEST_CASE("HtmlToMarkdown: <pre><code> code block") {
    CHECK(Md("<pre><code>x=1</code></pre>") == "\n```\n`x=1`\n```\n\n");
}

TEST_CASE("HtmlToMarkdown: <span> passthrough") { CHECK(Md("<span>text</span>") == "text"); }

TEST_CASE("HtmlToMarkdown: table") {
    CHECK(Md("<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>") ==
          "\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n");
}

TEST_CASE("HtmlToMarkdown: table with pipe escaping") {
    CHECK(Md("<table><tr><td>a|b</td></tr></table>") == "\n\n| a\\|b |\n| --- |\n\n");
}

TEST_CASE("HtmlToMarkdown: unknown tag trips fallback") {
    bool fell = false;
    CHECK(Md("<marquee>hi</marquee>", fell) == "hi");
    CHECK(fell == true);
}

TEST_CASE("HtmlToMarkdown: known tags do not trip fallback") {
    bool fell = true;
    Md("<p>hi</p>", fell);
    CHECK(fell == false);
}

TEST_CASE("HtmlToMarkdown: nested table trips fallback") {
    bool fell = false;
    Md("<table><tr><td><table><tr><td>x</td></tr></table></td></tr></table>", fell);
    CHECK(fell == true);
}

TEST_CASE("HtmlToMarkdown: comment is ignored") { CHECK(Md("a<!-- comment -->b") == "ab"); }

TEST_CASE("HtmlToMarkdown: collapses 3+ newlines") { CHECK(Md("<p>a</p><p>b</p><p>c</p>") == "a\n\nb\n\nc\n\n"); }

TEST_CASE("HtmlToMarkdown: mixed inline within paragraph") {
    CHECK(Md("<p>Hello <strong>bold</strong> and <em>italic</em></p>") == "Hello **bold** and *italic*\n\n");
}
