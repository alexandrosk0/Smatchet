#include "AiChatTextEditorRender.h"

#include "TextEditor.h"

#include <string>

namespace {

// Build a chat-specific palette derived from `GetDarkPalette()` with two
// overrides so the chat view blends with the surrounding Smatchet theme
// instead of looking like a code editor:
//   1. `Default` (plain body text) bumped to a bright off-white so prose is
//      readable against the panel's dark background — the original dark
//      palette greys it out to 0x7f7f7f which suited code but not prose.
//   2. `Background` cleared to transparent so the parent `BeginChild`'s
//      colour shows through.
TextEditor::Palette BuildChatPalette() {
    TextEditor::Palette p = TextEditor::GetDarkPalette();
    using P = TextEditor::PaletteIndex;
    p[(int)P::Default] = 0xffe0e0e0;    // bright off-white body text
    p[(int)P::Background] = 0x00000000; // transparent — parent panel bg shows
    return p;
}

// Append a role-prefixed message body to `out`, separating turns with a
// blank line. The leading "> Role:" line is matched by the MarkdownChat()
// LD's role-line regex so role transitions stand out visually.
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
    // Use the native LD colorizer with the markdown-chat language definition.
    editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::MarkdownChat());
    editor_->SetColorizerEnable(true);
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

    if (wasAtTail) {
        editor_->MoveBottom();
    }
}

void AiChatTextEditorView::Draw(float availW, float availH) {
    editor_->Render("##ai_chat_editor", ImVec2(availW, availH), false);
}
