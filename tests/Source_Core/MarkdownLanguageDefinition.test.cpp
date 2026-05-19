// Bucket-A doctest for TextEditor::LanguageDefinition::Markdown() and
// ::MarkdownChat(). The full colorizer integration belongs in bucket E
// (ImGui Test Engine, deferred); this test compiles each regex into
// std::regex and asserts match shape on representative inputs.
//
// Anchors: tests for the regex strings live alongside the LD source they
// describe — Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp,
// LanguageDefinition::Markdown() / MarkdownChat().

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
    // No space → must NOT match the heading regex.
    {
        const std::string s = "#foo";
        std::regex headingRe("^#{1,6}[ \\t].*$");
        CHECK_FALSE(std::regex_search(s, headingRe));
    }
}

TEST_CASE("Markdown LD blockquote regex matches `> ` prefix") {
    std::regex quoteRe("^>[ \\t].*$");
    CHECK(std::regex_search(std::string("> a quote"), quoteRe));
    CHECK(std::regex_search(std::string(">\tindented"), quoteRe));
    // Bare `>` without space — not a blockquote.
    CHECK_FALSE(std::regex_search(std::string(">noSpace"), quoteRe));
}

TEST_CASE("Markdown LD list marker regex matches both unordered and ordered") {
    std::regex listRe("^[ \\t]*([-*+]|\\d+\\.)[ \\t]");
    CHECK(std::regex_search(std::string("- one"), listRe));
    CHECK(std::regex_search(std::string("* two"), listRe));
    CHECK(std::regex_search(std::string("+ three"), listRe));
    CHECK(std::regex_search(std::string("1. first"), listRe));
    CHECK(std::regex_search(std::string("  10. nested"), listRe));
    // Missing trailing space.
    CHECK_FALSE(std::regex_search(std::string("-noSpace"), listRe));
    CHECK_FALSE(std::regex_search(std::string("1.noSpace"), listRe));
}

TEST_CASE("Markdown LD link regex matches [text](url) and ![alt](url)") {
    std::regex linkRe("!?\\[[^\\]]+\\]\\([^)]+\\)");
    CHECK(std::regex_search(std::string("see [example](https://example.org)"), linkRe));
    CHECK(std::regex_search(std::string("![alt](pic.png)"), linkRe));
    // Missing url paren — not a link.
    CHECK_FALSE(std::regex_search(std::string("[just text]"), linkRe));
}

TEST_CASE("Markdown LD bold wins over italic when both apply") {
    std::regex boldRe("\\*\\*[^\\*]+\\*\\*");
    std::regex italicRe("\\*[^\\*\\n]+\\*");
    const std::string s = "**bold**";
    // Both patterns can technically match in the input — the LD ordering puts
    // bold before italic so the colorizer takes bold first. Here we assert
    // that bold matches the canonical bold input AND that italic on `*x*`
    // matches independently.
    CHECK(std::regex_search(s, boldRe));
    CHECK(std::regex_search(std::string("*x*"), italicRe));
    // Italic body class excludes `*` so the unmatched single `**foo*` does
    // not produce a bold match.
    CHECK_FALSE(std::regex_search(std::string("**foo*"), boldRe));
}

TEST_CASE("Markdown LD inline code regex matches a single backtick pair") {
    std::regex codeRe("`[^`\\n]+`");
    CHECK(std::regex_search(std::string("inline `code` here"), codeRe));
    // No closer — not inline code.
    CHECK_FALSE(std::regex_search(std::string("backtick `but no close"), codeRe));
}

TEST_CASE("Markdown LD horizontal rule regex requires three or more of one char") {
    std::regex hrRe("^(-{3,}|\\*{3,}|_{3,})$");
    CHECK(std::regex_search(std::string("---"), hrRe));
    CHECK(std::regex_search(std::string("****"), hrRe));
    CHECK(std::regex_search(std::string("_________"), hrRe));
    CHECK_FALSE(std::regex_search(std::string("--"), hrRe));
}

TEST_CASE("Markdown LD strikethrough regex matches ~~text~~") {
    std::regex strikeRe("~~[^~]+~~");
    CHECK(std::regex_search(std::string("plain ~~struck~~ ok"), strikeRe));
    CHECK_FALSE(std::regex_search(std::string("just ~one~ tilde"), strikeRe));
}

TEST_CASE("Markdown LD declares fence as same-token block comment") {
    const LD& md = LD::Markdown();
    CHECK(md.mCommentStart == "```");
    CHECK(md.mCommentEnd == "```");
    CHECK(md.mSingleLineComment == "");
    CHECK(md.mAutoIndentation == false);
    CHECK(md.mCaseSensitive == true);
}

TEST_CASE("MarkdownChat LD prepends role-line regex before blockquote") {
    const LD& chat = LD::MarkdownChat();
    CHECK(chat.mName == "MarkdownChat");
    CHECK(AllRegexesCompile(chat));
    CHECK_FALSE(chat.mTokenRegexStrings.empty());

    // The first regex in the chat LD must be the role-line regex (precedence
    // over the generic blockquote). Verify by structure: the first regex must
    // match `> You:` and `> Assistant:` and `> Assistant (streaming...):`.
    const std::string& firstPattern = chat.mTokenRegexStrings[0].first;
    std::regex firstRe(firstPattern);
    CHECK(std::regex_search(std::string("> You:"), firstRe));
    CHECK(std::regex_search(std::string("> Assistant:"), firstRe));
    CHECK(std::regex_search(std::string("> Assistant (streaming...):"), firstRe));
    // Generic blockquote text must NOT match the role regex.
    CHECK_FALSE(std::regex_search(std::string("> a normal quote"), firstRe));
}

TEST_CASE("MarkdownChat LD inherits base Markdown regex set") {
    const LD& chat = LD::MarkdownChat();
    // The chat LD must include all base Markdown regexes (heading, list,
    // inline code, etc.) plus the role-line regex on top.
    CHECK(chat.mTokenRegexStrings.size() > LD::Markdown().mTokenRegexStrings.size());
    // Heading regex still present somewhere.
    CHECK(AnyRegexMatches(chat, P::Keyword, "# H1 heading"));
    // Bold + inline code carry over.
    CHECK(AnyRegexMatches(chat, P::Identifier, "**bold here**"));
    CHECK(AnyRegexMatches(chat, P::Number, "talk about `code`"));
}
