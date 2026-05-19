#include "AiChatTextEditorRender.h"

#include "AiChatMarkdownTokens.h"
#include "SmatchetThemedTextEditorPalette.h"
#include "TextEditor.h"

#include <algorithm>
#include <string>

namespace {

// Chat palette is theme-aware now — derived from
// SmatchetTheme::GetThemedAiChatPalette() which reads the live ImGui style +
// SmatchetTheme syntax-color palette. We re-apply per Draw() so theme switches
// propagate without a separate notify path (~84 bytes of array copy + one
// SetPalette() = a handful of cache lines, negligible vs the frame draw cost).
// See SmatchetThemedTextEditorPalette.h for the override rationale.

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
    // Initial palette — Draw() re-applies every frame so theme switches
    // propagate. Seed it here too so the first frame isn't drawn with the
    // third-party widget's hard-coded TextEditor::GetDarkPalette() default
    // before our per-Draw refresh runs.
    editor_->SetPalette(SmatchetTheme::GetThemedAiChatPalette());
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
    // Refresh the palette every frame so a SmatchetTheme switch (driven by
    // SmatchetUI::Draw -> SmatchetTheme::ApplyStyle) propagates instantly to
    // this embedded TextEditor instance. The third-party widget caches its
    // palette in mPaletteBase + does not subscribe to theme changes, so the
    // only way to stay in sync without adding an observer protocol is to
    // re-push the desired palette on every render. ~84 bytes of array copy +
    // one SetPalette() per frame; negligible against the ImGui draw cost.
    editor_->SetPalette(SmatchetTheme::GetThemedAiChatPalette());
    editor_->Render("##ai_chat_editor", ImVec2(availW, availH), false);
}
