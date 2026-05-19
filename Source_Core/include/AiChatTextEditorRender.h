#ifndef SMATCHET_AI_CHAT_TEXT_EDITOR_RENDER_H
#define SMATCHET_AI_CHAT_TEXT_EDITOR_RENDER_H

#include "AiTypes.h"

#include <memory>
#include <string>
#include <vector>

class TextEditor;

// One read-only ImGuiColorTextEdit instance owned per AI assistant panel.
// Replaces the previous per-message ImGui::InputTextMultiline render so that
// drag-select + Ctrl+C / Ctrl+A work across messages. Markdown is coloured
// in-place by the hand-rolled AiChatMarkdownTokens scanner; the whole render
// stays monospace (TextEditor uses ImGui's default monospaced font).
class AiChatTextEditorView {
  public:
    AiChatTextEditorView();
    ~AiChatTextEditorView();
    AiChatTextEditorView(const AiChatTextEditorView&) = delete;
    AiChatTextEditorView& operator=(const AiChatTextEditorView&) = delete;

    // Rebuild the editor buffer from the conversation snapshot. Cheap when
    // the serialised content is unchanged (string-equality short-circuit).
    // `wasAtTail` carries the parent-child scroll state across the call so
    // auto-scroll-to-tail behaviour mirrors the prior InputTextMultiline
    // path.
    void RebuildBuffer(const std::vector<AiMessage>& history, const std::string& streamingBuf, bool inFlight,
                       bool wasAtTail);

    // Render inside the current ImGui scope.
    void Draw(float availW, float availH);

  private:
    std::unique_ptr<TextEditor> editor_;
    std::string lastSerialised_;
};

#endif // SMATCHET_AI_CHAT_TEXT_EDITOR_RENDER_H
