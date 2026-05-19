// AiChatMarkdownTokens pure-logic tokenizer tests. Covers the documented
// edge-case list in docs/design/ai-chat-textedit-markdown.md.
//
// No ImGui / TextEditor / md4c dependency — the tokenizer emits structural
// (line, colStart, colEnd, kind) spans that the view layer translates into
// palette writes.

#include "AiChatMarkdownTokens.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using AiChatMarkdownTokens::Tokenize;
using AiChatMarkdownTokens::TokenKind;
using AiChatMarkdownTokens::TokenSpan;

// Returns the count of spans of `kind` in `spans`.
int CountKind(const std::vector<TokenSpan>& spans, TokenKind kind) {
    return static_cast<int>(
        std::count_if(spans.begin(), spans.end(), [kind](const TokenSpan& s) { return s.kind == kind; }));
}

// Returns the first span of `kind`, or a sentinel {-1,-1,-1,Plain}.
TokenSpan FirstOfKind(const std::vector<TokenSpan>& spans, TokenKind kind) {
    const auto it = std::find_if(spans.begin(), spans.end(), [kind](const TokenSpan& s) { return s.kind == kind; });
    if (it != spans.end()) {
        return *it;
    }
    TokenSpan miss;
    miss.line = -1;
    miss.colStart = -1;
    miss.colEnd = -1;
    miss.kind = TokenKind::Plain;
    return miss;
}

} // namespace

TEST_CASE("AiChatMarkdownTokens empty input emits no spans") {
    const std::vector<TokenSpan> spans = Tokenize("");
    CHECK(spans.empty());
}

TEST_CASE("AiChatMarkdownTokens whitespace-only input emits no spans") {
    const std::vector<TokenSpan> spans = Tokenize("   \n  \n");
    CHECK(spans.empty());
}

TEST_CASE("AiChatMarkdownTokens H1 heading emits marker + text") {
    const std::vector<TokenSpan> spans = Tokenize("# Hello world");
    CHECK(CountKind(spans, TokenKind::HeadingMarker) == 1);
    CHECK(CountKind(spans, TokenKind::HeadingText) == 1);
    const TokenSpan mk = FirstOfKind(spans, TokenKind::HeadingMarker);
    CHECK(mk.line == 0);
    CHECK(mk.colStart == 0);
    CHECK(mk.colEnd == 2); // "# "
    const TokenSpan tx = FirstOfKind(spans, TokenKind::HeadingText);
    CHECK(tx.line == 0);
    CHECK(tx.colStart == 2);
    CHECK(tx.colEnd == 13); // "Hello world"
}

TEST_CASE("AiChatMarkdownTokens H1..H6 headings all recognised") {
    for (int level = 1; level <= 6; ++level) {
        const std::string prefix(static_cast<std::size_t>(level), '#');
        const std::string md = prefix + " heading";
        const std::vector<TokenSpan> spans = Tokenize(md);
        CHECK(CountKind(spans, TokenKind::HeadingMarker) == 1);
        CHECK(CountKind(spans, TokenKind::HeadingText) == 1);
        const TokenSpan mk = FirstOfKind(spans, TokenKind::HeadingMarker);
        CHECK(mk.colEnd == level + 1); // #{1..6} + ' '
    }
}

TEST_CASE("AiChatMarkdownTokens H7 is NOT a heading") {
    const std::vector<TokenSpan> spans = Tokenize("####### too many");
    CHECK(CountKind(spans, TokenKind::HeadingMarker) == 0);
    CHECK(CountKind(spans, TokenKind::HeadingText) == 0);
}

TEST_CASE("AiChatMarkdownTokens paragraph with bold + italic + inline code") {
    const std::vector<TokenSpan> spans = Tokenize("hello **bold** and *italic* with `code` end");
    CHECK(CountKind(spans, TokenKind::BoldMarker) == 2);
    CHECK(CountKind(spans, TokenKind::BoldText) == 1);
    CHECK(CountKind(spans, TokenKind::ItalicMarker) == 2);
    CHECK(CountKind(spans, TokenKind::ItalicText) == 1);
    CHECK(CountKind(spans, TokenKind::InlineCode) == 1);
}

TEST_CASE("AiChatMarkdownTokens fenced code: lang tag + body suppresses inline") {
    const std::string md = "```cpp\nint x = **not bold**;\n```";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::FenceMarker) == 2);
    CHECK(CountKind(spans, TokenKind::CodeFenceBody) == 1);
    // Inside the fence body, the "**not bold**" sequence must NOT tokenise as
    // bold — the body emits only CodeFenceBody.
    CHECK(CountKind(spans, TokenKind::BoldMarker) == 0);
    CHECK(CountKind(spans, TokenKind::BoldText) == 0);
}

TEST_CASE("AiChatMarkdownTokens unclosed fence stays open to EOF") {
    const std::string md = "```py\nstuff\nmore stuff\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::FenceMarker) == 1);
    // Two body lines + one (empty) trailing line emitted after the final \n.
    CHECK(CountKind(spans, TokenKind::CodeFenceBody) >= 2);
}

TEST_CASE("AiChatMarkdownTokens unordered list emits ListMarker per item") {
    const std::string md = "- one\n* two\n+ three\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::ListMarker) == 3);
}

TEST_CASE("AiChatMarkdownTokens ordered list emits ListMarker") {
    const std::string md = "1. first\n2. second\n10. tenth\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::ListMarker) == 3);
}

TEST_CASE("AiChatMarkdownTokens blockquote emits QuoteMarker; multi-line") {
    const std::string md = "> first quote\n> second line\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::QuoteMarker) == 2);
}

TEST_CASE("AiChatMarkdownTokens link emits LinkText + LinkUrl") {
    const std::vector<TokenSpan> spans = Tokenize("see [example](https://example.org) here");
    CHECK(CountKind(spans, TokenKind::LinkText) == 1);
    CHECK(CountKind(spans, TokenKind::LinkUrl) == 1);
    const TokenSpan tx = FirstOfKind(spans, TokenKind::LinkText);
    const TokenSpan url = FirstOfKind(spans, TokenKind::LinkUrl);
    CHECK(tx.colEnd <= url.colStart); // url follows text in source order
}

TEST_CASE("AiChatMarkdownTokens strike emits Strike span") {
    const std::vector<TokenSpan> spans = Tokenize("normal ~~struck~~ end");
    CHECK(CountKind(spans, TokenKind::Strike) == 1);
}

TEST_CASE("AiChatMarkdownTokens mixed sequence heading + para + fence + list") {
    const std::string md = "# Title\n"
                           "\n"
                           "Some **bold** intro.\n"
                           "\n"
                           "```\n"
                           "code\n"
                           "```\n"
                           "\n"
                           "- bullet one\n"
                           "- bullet two\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::HeadingMarker) == 1);
    CHECK(CountKind(spans, TokenKind::HeadingText) == 1);
    CHECK(CountKind(spans, TokenKind::BoldMarker) == 2);
    CHECK(CountKind(spans, TokenKind::BoldText) == 1);
    CHECK(CountKind(spans, TokenKind::FenceMarker) == 2);
    CHECK(CountKind(spans, TokenKind::CodeFenceBody) >= 1);
    CHECK(CountKind(spans, TokenKind::ListMarker) == 2);
}

TEST_CASE("AiChatMarkdownTokens bold vs italic disambiguation") {
    // `*x*` is italic; `**x**` is bold; bare-text *no-italic-here lacks a close.
    const std::vector<TokenSpan> aSpans = Tokenize("*x*");
    CHECK(CountKind(aSpans, TokenKind::ItalicMarker) == 2);
    CHECK(CountKind(aSpans, TokenKind::ItalicText) == 1);
    CHECK(CountKind(aSpans, TokenKind::BoldMarker) == 0);

    const std::vector<TokenSpan> bSpans = Tokenize("**x**");
    CHECK(CountKind(bSpans, TokenKind::BoldMarker) == 2);
    CHECK(CountKind(bSpans, TokenKind::BoldText) == 1);
    // Bold ran first; the * around `**x**` are part of the BoldMarker spans,
    // so no italic spans should leak through.
    CHECK(CountKind(bSpans, TokenKind::ItalicMarker) == 0);
}

TEST_CASE("AiChatMarkdownTokens role-prefix conversation tokenises as quote") {
    // The view layer concatenates messages with "> Role:" prefixes. Verify the
    // tokeniser sees those as QuoteMarker spans so role transitions stand out.
    const std::string md = "> You:\n"
                           "hi\n"
                           "\n"
                           "> Assistant:\n"
                           "hello\n";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::QuoteMarker) == 2);
}

TEST_CASE("AiChatMarkdownTokens backtick pair is non-greedy across newlines") {
    // `[^`]+` should not match across a newline: the backtick on line 0 has no
    // closer on the same line, so no InlineCode span is emitted.
    const std::string md = "no `start here\nand continue` end";
    const std::vector<TokenSpan> spans = Tokenize(md);
    CHECK(CountKind(spans, TokenKind::InlineCode) == 0);
}
