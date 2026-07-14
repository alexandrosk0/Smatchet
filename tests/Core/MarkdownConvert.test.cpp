#include <doctest/doctest.h>

#include "MarkdownConvert.h"

#include <string>

using MarkdownConvert::HtmlSubsetToMarkdown;
using MarkdownConvert::MarkdownToHtml;

namespace {

// Convenience: convert and ignore the fell-back flag.
std::string Md(const std::string& html) { return HtmlSubsetToMarkdown(html, nullptr); }

// Convert and capture the fell-back flag.
std::string Md(const std::string& html, bool& fell) { return HtmlSubsetToMarkdown(html, &fell); }

// Convenience: Markdown -> HTML-subset (the Plane `description_html` producer).
std::string Html(const std::string& md) { return MarkdownToHtml(md); }

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

// ---------------------------------------------------------------------------
// ParseHtmlTag attribute / token-shape goldens, exercised through the public
// HtmlSubsetToMarkdown surface. These pin the attribute-loop helper extraction
// (ConsumeHtmlTagName / ParseOneHtmlAttribute / ApplyHtmlAttribute /
// ParseHtmlBangOrPi) — output is byte-for-byte identical to the inline parser.
// ---------------------------------------------------------------------------

TEST_CASE("ParseHtmlTag: single-quoted href value") {
    CHECK(Md("<a href='http://e.com'>link</a>") == "[link](http://e.com)");
}

TEST_CASE("ParseHtmlTag: unquoted href value stops at first '/'") {
    // Unquoted attribute values terminate at '/' (the void-tag self-close marker),
    // so a bare URL is truncated at the first slash. Pinned as-is to preserve the
    // pre-refactor parser's byte-for-byte behaviour.
    CHECK(Md("<a href=http://e.com>link</a>") == "[link](http:)");
}

TEST_CASE("ParseHtmlTag: href entity-decoded") {
    CHECK(Md("<a href=\"http://e.com/a&amp;b\">link</a>") == "[link](http://e.com/a&b)");
}

TEST_CASE("ParseHtmlTag: extra ignored attribute before href") {
    CHECK(Md("<a class=\"btn\" href=\"http://e.com\">link</a>") == "[link](http://e.com)");
}

TEST_CASE("ParseHtmlTag: img src and alt attributes") { CHECK(Md("<img alt=\"a\" src=\"u.png\">") == "![a](u.png)"); }

TEST_CASE("ParseHtmlTag: self-closed void img") { CHECK(Md("<img src=\"u.png\" alt=\"a\"/>") == "![a](u.png)"); }

TEST_CASE("ParseHtmlTag: uppercase tag name lowercased") { CHECK(Md("<STRONG>x</STRONG>") == "**x**"); }

TEST_CASE("ParseHtmlTag: bang declaration ignored") { CHECK(Md("<!DOCTYPE html>text") == "text"); }

TEST_CASE("ParseHtmlTag: processing instruction ignored") { CHECK(Md("<?xml version=\"1.0\"?>text") == "text"); }

TEST_CASE("ParseHtmlTag: lone '<' is literal when no tag follows") { CHECK(Md("a < b") == "a < b"); }

// ---------------------------------------------------------------------------
// MarkdownToHtml — the Plane `description_html` producer (md4c HTML renderer
// via Md4cParserFlags()). Each case pins the byte-for-byte output for one
// construct so a future md4c bump or flag change surfaces as a golden diff.
// Goldens were captured from the live MarkdownConvert::MarkdownToHtml over the
// vendored md4c release-0.5.2. Sibling coverage: HtmlSubsetToMarkdown above
// (the inverse) and MarkdownConvertAdf.test.cpp (the ADF pair).
// ---------------------------------------------------------------------------

TEST_CASE("MarkdownToHtml: paragraph") {
    CHECK(Html("Hello world.") == "<p>Hello world.</p>");
    CHECK(Html("one\n\ntwo\n\nthree") == "<p>one</p><p>two</p><p>three</p>");
    CHECK(Html("") == "");
}

TEST_CASE("MarkdownToHtml: inline emphasis") {
    CHECK(Html("**bold** and *italic*") == "<p><strong>bold</strong> and <em>italic</em></p>");
    CHECK(Html("***both***") == "<p><em><strong>both</strong></em></p>");
    CHECK(Html("*a **b** c*") == "<p><em>a <strong>b</strong> c</em></p>");
    CHECK(Html("~~gone~~") == "<p><s>gone</s></p>");
}

TEST_CASE("MarkdownToHtml: headings h1-h6") {
    CHECK(Html("# Title\n\nBody text.") == "<h1>Title</h1><p>Body text.</p>");
    CHECK(Html("###### Deep") == "<h6>Deep</h6>");
}

TEST_CASE("MarkdownToHtml: unordered list") {
    CHECK(Html("- one\n- two\n- three") == "<ul><li>one</li><li>two</li><li>three</li></ul>");
}

TEST_CASE("MarkdownToHtml: nested unordered list") {
    CHECK(Html("- a\n    - a1\n    - a2\n- b") ==
          "<ul><li>a<ul><li>a1</li><li>a2</li></ul></li><li>b</li></ul>");
}

TEST_CASE("MarkdownToHtml: ordered list") {
    CHECK(Html("1. first\n2. second") == "<ol><li>first</li><li>second</li></ol>");
    // A non-1 starting index carries through as the `start` attribute.
    CHECK(Html("3. three\n4. four") == "<ol start=\"3\"><li>three</li><li>four</li></ol>");
}

TEST_CASE("MarkdownToHtml: inline code and fenced block") {
    CHECK(Html("Use `printf` here.") == "<p>Use <code>printf</code> here.</p>");
    CHECK(Html("```\nint x = 1;\n```") == "<pre><code>int x = 1;\n</code></pre>");
    // Angle brackets inside code are HTML-escaped, not treated as tags.
    CHECK(Html("`a<b>c`") == "<p><code>a&lt;b&gt;c</code></p>");
}

TEST_CASE("MarkdownToHtml: links and autolinks") {
    CHECK(Html("See [Jira](https://jira.example.com).") ==
          "<p>See <a href=\"https://jira.example.com\">Jira</a>.</p>");
    CHECK(Html("<https://ex.com>") == "<p><a href=\"https://ex.com\">https://ex.com</a></p>");
}

TEST_CASE("MarkdownToHtml: blockquote") {
    CHECK(Html("> quoted line") == "<blockquote><p>quoted line</p></blockquote>");
}

TEST_CASE("MarkdownToHtml: thematic break") {
    CHECK(Html("text\n\n---\n\nmore") == "<p>text</p><hr/><p>more</p>");
}

TEST_CASE("MarkdownToHtml: line breaks collapse to <br/>") {
    // Both hard (trailing-two-space) and soft breaks render as <br/> under the
    // pipeline's parser flags — see Md4cParserFlags().
    CHECK(Html("line1  \nline2") == "<p>line1<br/>line2</p>");
    CHECK(Html("line1\nline2") == "<p>line1<br/>line2</p>");
}

TEST_CASE("MarkdownToHtml: table") {
    CHECK(Html("| A | B |\n|---|---|\n| 1 | 2 |") ==
          "<table><thead><tr><th>A</th><th>B</th></tr></thead>"
          "<tbody><tr><td>1</td><td>2</td></tr></tbody></table>");
}

TEST_CASE("MarkdownToHtml: special characters are HTML-escaped") {
    CHECK(Html("5 < 6 & 7 > 4") == "<p>5 &lt; 6 &amp; 7 &gt; 4</p>");
    CHECK(Html("Tom & Jerry") == "<p>Tom &amp; Jerry</p>");
}
