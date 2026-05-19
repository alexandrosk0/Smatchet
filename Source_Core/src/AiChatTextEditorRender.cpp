#include "AiChatTextEditorRender.h"

#include "AiChatMarkdownTokens.h"
#include "TextEditor.h"

#include <algorithm>
#include <string>

namespace {

// Build a chat-specific palette derived from `GetDarkPalette()` with three
// overrides so the chat view blends with the surrounding Smatchet theme
// instead of looking like a code editor:
//   1. `Default` (Plain body text) bumped to a bright off-white so prose is
//      readable against the panel's dark background — the original dark
//      palette greys it out to 0x7f7f7f which suited code but not prose.
//   2. `Background` cleared to transparent so the parent `BeginChild`'s
//      colour shows through.
//   3. Two unused-by-the-markdown-mapping slots (CharLiteral,
//      PreprocIdentifier) repurposed for the chat role overlays:
//      CharLiteral = user-role marker tint, PreprocIdentifier = user-body
//      tint. Claude-desktop-style differentiation without needing a new
//      PaletteIndex slot in the third-party widget.
TextEditor::Palette BuildChatPalette() {
    TextEditor::Palette p = TextEditor::GetDarkPalette();
    using P = TextEditor::PaletteIndex;
    p[(int)P::Default] = 0xffe0e0e0;           // bright off-white body text
    p[(int)P::Background] = 0x00000000;        // transparent — parent panel bg shows
    p[(int)P::CharLiteral] = 0xfff5c97a;       // soft amber for user role lines
    p[(int)P::PreprocIdentifier] = 0xffd9a76a; // deeper amber for user msg body
    return p;
}

// Map AiChatMarkdownTokens::TokenKind onto TextEditor::PaletteIndex. Most
// kinds reuse the dark palette's existing slots; the three chat-overlay kinds
// (UserRoleMarker, AssistantRoleMarker, UserMsgBody) hit the slots we
// repurposed in `BuildChatPalette` so user messages render in amber while
// assistant messages stay in the default theme color.
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
        return P::KnownIdentifier;
    case TK::LinkUrl:
        return P::Comment;
    case TK::ListMarker:
        return P::Punctuation;
    case TK::QuoteMarker:
        return P::Keyword;
    case TK::Strike:
        return P::Preprocessor;
    case TK::UserRoleMarker:
        return P::CharLiteral;
    case TK::AssistantRoleMarker:
        return P::Keyword;
    case TK::UserMsgBody:
        return P::PreprocIdentifier;
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
    // Hide the line-number gutter — this view is prose, not code.
    editor_->SetShowLineNumbers(false);
    editor_->SetPalette(BuildChatPalette());
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
