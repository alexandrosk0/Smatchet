#ifndef SMATCHET_AI_CHAT_MARKDOWN_TOKENS_H
#define SMATCHET_AI_CHAT_MARKDOWN_TOKENS_H

#include <string>
#include <vector>

// Pure-logic markdown tokenizer for the AI chat panel.
//
// Hand-rolled line + regex scanner. No md4c, no ImGui, no TextEditor deps —
// the scanner returns lightweight {line, colStart, colEnd, kind} spans that
// the view layer translates into TextEditor::PaletteIndex writes. The doctest
// rig links this TU alone (no UI dependencies) so unit coverage is cheap.
//
// md4c is unsuitable here because its block / span callbacks do not expose
// source byte offsets — only parsed text bytes. The TextEditor render path
// needs source-byte ranges to colour glyphs in place, hence the hand-rolled
// scanner.
namespace AiChatMarkdownTokens {

enum class TokenKind : int {
    Plain = 0,
    HeadingMarker,
    HeadingText,
    FenceMarker,
    CodeFenceBody,
    InlineCode,
    BoldMarker,
    BoldText,
    ItalicMarker,
    ItalicText,
    LinkText,
    LinkUrl,
    ListMarker,
    QuoteMarker,
    Strike,
};

struct TokenSpan {
    int line;     // 0-based
    int colStart; // byte offset within line (inclusive)
    int colEnd;   // byte offset within line (exclusive)
    TokenKind kind;
};

// Tokenise the markdown blob. The output is in document order and
// non-overlapping; gaps between consecutive spans are implicitly Plain.
std::vector<TokenSpan> Tokenize(const std::string& md);

} // namespace AiChatMarkdownTokens

#endif // SMATCHET_AI_CHAT_MARKDOWN_TOKENS_H
