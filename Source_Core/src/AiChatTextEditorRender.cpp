#include "AiChatTextEditorRender.h"

#include "AiChatMarkdownTokens.h"
#include "TextEditor.h"

#include <algorithm>
#include <string>

namespace {

// Map AiChatMarkdownTokens::TokenKind onto TextEditor::PaletteIndex per the
// table in docs/design/ai-chat-textedit-markdown.md. Reuses GetDarkPalette()
// values so the colour scheme matches the Lua editor look out of the box.
TextEditor::PaletteIndex MapPalette(AiChatMarkdownTokens::TokenKind k) {
    using TK = AiChatMarkdownTokens::TokenKind;
    using P = TextEditor::PaletteIndex;
    switch (k) {
    case TK::HeadingMarker:
    case TK::HeadingText:
        return P::Keyword;
    case TK::FenceMarker:
    case TK::CodeFenceBody:
        return P::String;
    case TK::InlineCode:
        return P::Number;
    case TK::BoldMarker:
    case TK::BoldText:
        return P::Identifier;
    case TK::ItalicMarker:
    case TK::ItalicText:
        return P::Comment;
    case TK::LinkText:
        // PaletteIndex has no `Function`; KnownIdentifier renders cyan in the
        // dark palette which matches the intended "links pop" colour.
        return P::KnownIdentifier;
    case TK::LinkUrl:
        return P::Comment;
    case TK::ListMarker:
        return P::Punctuation;
    case TK::QuoteMarker:
        return P::Keyword;
    case TK::Strike:
        return P::Preprocessor;
    case TK::Plain:
    default:
        return P::Default;
    }
}

// Append a role-prefixed message body to `out`, separating turns with a
// blank line. The leading "> Role:" line tokenises as a QuoteMarker so role
// transitions stand out visually.
void AppendTurn(std::string& out, const char* role, const std::string& body) {
    if (!out.empty()) {
        out.push_back('\n');
    }
    out.append("> ");
    out.append(role);
    out.append(":\n");
    out.append(body);
    if (!body.empty() && body.back() != '\n') {
        out.push_back('\n');
    }
}

} // namespace

AiChatTextEditorView::AiChatTextEditorView() : editor_(new TextEditor()) {
    editor_->SetReadOnly(true);
    editor_->SetShowWhitespaces(false);
    editor_->SetPalette(TextEditor::GetDarkPalette());
    // Parent panel already owns the scrolling BeginChild; let TextEditor skip
    // its internal BeginChild so the parent retains scroll authority and the
    // auto-scroll-to-tail logic below remains accurate.
    editor_->SetImGuiChildIgnored(true);
    // Manual colouring only — ColorizeInternal()'s regex pass would otherwise
    // overwrite our spans every frame.
    editor_->DisableColorizerPasses();
}

AiChatTextEditorView::~AiChatTextEditorView() = default;

void AiChatTextEditorView::RebuildBuffer(const std::vector<AiMessage>& history, const std::string& streamingBuf,
                                         bool inFlight, bool wasAtTail) {
    std::string serialised;
    serialised.reserve(512);
    for (std::size_t i = 0; i < history.size(); ++i) {
        const AiMessage& m = history[i];
        const bool isUser = (m.Role == "user");
        AppendTurn(serialised, isUser ? "You" : "Assistant", m.Content);
    }
    if (inFlight && !streamingBuf.empty()) {
        AppendTurn(serialised, "Assistant (streaming...)", streamingBuf);
    }

    if (serialised == lastSerialised_) {
        return;
    }
    lastSerialised_ = serialised;

    editor_->SetText(serialised);
    // SetText() can re-enable colourisation through ColorizeInternal scheduling;
    // make sure our explicit spans survive.
    editor_->DisableColorizerPasses();

    const std::vector<AiChatMarkdownTokens::TokenSpan> spans = AiChatMarkdownTokens::Tokenize(serialised);
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const AiChatMarkdownTokens::TokenSpan& s = spans[i];
        editor_->SetTokenColor(s.line, s.colStart, s.colEnd, MapPalette(s.kind));
    }

    if (wasAtTail) {
        editor_->MoveBottom();
    }
}

void AiChatTextEditorView::Draw(float availW, float availH) {
    editor_->Render("##ai_chat_editor", ImVec2(availW, availH), false);
}
