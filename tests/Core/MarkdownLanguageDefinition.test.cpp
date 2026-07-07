// Bucket-A doctest for TextEditor::LanguageDefinition::Markdown(). The
// full colorizer integration belongs in bucket E (ImGui Test Engine,
// deferred); this test compiles each regex into std::regex and asserts
// match shape on representative inputs.
//
// MarkdownChat() was retired in Slice 5 when AI chat moved off TextEditor
// onto MarkdownPreviewRender + SelectableTextRun. Markdown() stays —
// still used by the plan-doc viewer + ticket description editor.
//
// Anchors: tests for the regex strings live alongside the LD source they
// describe — Source/Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp,
// LanguageDefinition::Markdown().

#include "TextEditor.h"

#include <doctest/doctest.h>

#include <regex>
#include <string>

namespace {

using LD = TextEditor::LanguageDefinition;
using P = TextEditor::PaletteIndex;

// Compile each (regex, PaletteIndex) pair in the language definition. Returns
// true if at least one regex with the given PaletteIndex matches `s`.
bool AnyRegexMatches(const LD& ld, P palette, const std::string& s) {
    for (std::size_t i = 0; i < ld.mTokenRegexStrings.size(); ++i) {
        const std::pair<std::string, P>& entry = ld.mTokenRegexStrings[i];
        if (entry.second != palette) {
            continue;
        }
        try {
            std::regex re(entry.first);
            if (std::regex_search(s, re)) {
                return true;
            }
        } catch (const std::regex_error&) {
            // A regex_error here means the LD ships an invalid pattern. Fail
            // the assertion loudly by reporting no match — the caller's CHECK
            // will surface it.
            return false;
        }
    }
    return false;
}

// Returns true if every regex in the language definition compiles cleanly.
bool AllRegexesCompile(const LD& ld) {
    for (std::size_t i = 0; i < ld.mTokenRegexStrings.size(); ++i) {
        try {
            std::regex re(ld.mTokenRegexStrings[i].first);
            (void)re;
        } catch (const std::regex_error&) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Markdown LD compiles every regex without throwing") {
    const LD& md = LD::Markdown();
    CHECK(AllRegexesCompile(md));
    CHECK(!md.mTokenRegexStrings.empty());
    CHECK(md.mName == "Markdown");
}

TEST_CASE("Markdown LD heading regex requires space after hashes") {
    const LD& md = LD::Markdown();
    // Valid heading levels 1..6 with space.
    CHECK(AnyRegexMatches(md, P::Keyword, "# H1"));
    CHECK(AnyRegexMatches(md, P::Keyword, "## H2"));
    CHECK(AnyRegexMatches(md, P::Keyword, "###### H6"));
    // No space → must NOT match the heading regex. Heading is the sole P::Keyword token.
    CHECK_FALSE(AnyRegexMatches(md, P::Keyword, "#foo"));
}

TEST_CASE("Markdown LD blockquote regex matches `> ` prefix") {
    const LD& md = LD::Markdown();
    // Blockquote is the sole P::Comment token in the production LD.
    CHECK(AnyRegexMatches(md, P::Comment, "> a quote"));
    CHECK(AnyRegexMatches(md, P::Comment, ">\tindented"));
    // Bare `>` without space — not a blockquote.
    CHECK_FALSE(AnyRegexMatches(md, P::Comment, ">noSpace"));
}

TEST_CASE("Markdown LD list marker regex matches both unordered and ordered") {
    const LD& md = LD::Markdown();
    // List markers are tagged P::Punctuation (shared with the horizontal-rule
    // token); none of the inputs below collide with the `^(-{3,}|…)$` rule regex.
    CHECK(AnyRegexMatches(md, P::Punctuation, "- one"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "* two"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "+ three"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "1. first"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "  10. nested"));
    // Missing trailing space.
    CHECK_FALSE(AnyRegexMatches(md, P::Punctuation, "-noSpace"));
    CHECK_FALSE(AnyRegexMatches(md, P::Punctuation, "1.noSpace"));
}

TEST_CASE("Markdown LD link regex matches [text](url) and ![alt](url)") {
    const LD& md = LD::Markdown();
    // Link/image is the sole P::KnownIdentifier token.
    CHECK(AnyRegexMatches(md, P::KnownIdentifier, "see [example](https://example.org)"));
    CHECK(AnyRegexMatches(md, P::KnownIdentifier, "![alt](pic.png)"));
    // Missing url paren — not a link.
    CHECK_FALSE(AnyRegexMatches(md, P::KnownIdentifier, "[just text]"));
}

TEST_CASE("Markdown LD bold wins over italic when both apply") {
    const LD& md = LD::Markdown();
    // Bold is P::Identifier, italic is P::String — distinct tokens. The LD orders
    // bold before italic so the colorizer takes bold first on `**…**`. Assert bold
    // matches the canonical bold input AND italic matches `*x*` independently.
    CHECK(AnyRegexMatches(md, P::Identifier, "**bold**"));
    CHECK(AnyRegexMatches(md, P::String, "*x*"));
    // Italic body class excludes `*` so the unmatched single `**foo*` does
    // not produce a bold match.
    CHECK_FALSE(AnyRegexMatches(md, P::Identifier, "**foo*"));
}

TEST_CASE("Markdown LD inline code regex matches a single backtick pair") {
    const LD& md = LD::Markdown();
    // Inline code is the sole P::Number token.
    CHECK(AnyRegexMatches(md, P::Number, "inline `code` here"));
    // No closer — not inline code.
    CHECK_FALSE(AnyRegexMatches(md, P::Number, "backtick `but no close"));
}

TEST_CASE("Markdown LD horizontal rule regex requires three or more of one char") {
    const LD& md = LD::Markdown();
    // Horizontal rule is tagged P::Punctuation (shared with the list-marker token);
    // the bare-rule inputs below are not matched by the list regex.
    CHECK(AnyRegexMatches(md, P::Punctuation, "---"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "****"));
    CHECK(AnyRegexMatches(md, P::Punctuation, "_________"));
    CHECK_FALSE(AnyRegexMatches(md, P::Punctuation, "--"));
}

TEST_CASE("Markdown LD strikethrough regex matches ~~text~~") {
    const LD& md = LD::Markdown();
    // Strikethrough is the sole P::Preprocessor token.
    CHECK(AnyRegexMatches(md, P::Preprocessor, "plain ~~struck~~ ok"));
    CHECK_FALSE(AnyRegexMatches(md, P::Preprocessor, "just ~one~ tilde"));
}

TEST_CASE("Markdown LD declares fence as same-token block comment") {
    const LD& md = LD::Markdown();
    CHECK(md.mCommentStart == "```");
    CHECK(md.mCommentEnd == "```");
    CHECK(md.mSingleLineComment == "");
    CHECK(md.mAutoIndentation == false);
    CHECK(md.mCaseSensitive == true);
}

TEST_CASE("Markdown LD same-token fence SetText round-trip smoke") {
    // The fence open/close regression (closing ``` mis-detected as a new
    // opener, leaving trailing content stuck inside the comment span) is
    // a colorizer-state bug — ColorizeInternal runs only from Render(),
    // which needs an ImGui context. The bucket-A surface we CAN cover here
    // is SetText round-trip on the canonical 4-line "opener / body /
    // closer / trailing prose" input that triggered the regression.
    // Catching a colorizer-induced corruption of the source buffer (or a
    // hang during SetText's mCheckComments scan) here is enough to flag
    // bigger breakages; the visual fence-stuck-open symptom itself is
    // validated under bucket E (ImGui Test Engine, deferred) or bucket B
    // (manual smoke). See plan § Verification.
    TextEditor ed;
    ed.SetLanguageDefinition(LD::Markdown());
    ed.SetColorizerEnable(true);
    const std::string md = "```markdown\n"
                           "# heading inside fence\n"
                           "```\n"
                           "trailing prose outside fence";
    ed.SetText(md);
    REQUIRE(ed.GetTotalLines() >= 4);
    const std::string roundTripped = ed.GetText();
    // TextEditor::GetText appends a trailing '\n' so compare on the prefix.
    REQUIRE(roundTripped.size() >= md.size());
    CHECK(roundTripped.compare(0, md.size(), md) == 0);
    // Re-SetText with a fence-free document must succeed (proxy for
    // "comment-span state machine is not corrupted").
    ed.SetText("plain prose no fence");
    CHECK(ed.GetTotalLines() == 1);
}
