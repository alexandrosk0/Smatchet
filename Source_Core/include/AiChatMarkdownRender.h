// AiChatMarkdownRender — block-level markdown render specifically for the AI
// Assistant chat panel. md4c parses the message into block units; each block
// renders into its own ReadOnly `ImGui::InputTextMultiline`, giving native
// drag-select + Ctrl+C semantics inside each block. Cross-block selection is
// not supported by ImGui; the AI panel layers a per-message "Copy entire
// message" button on top of this render to cover that case.
//
// The renderer flattens inline formatting (bold / italic / inline code /
// links) into raw markdown markers preserved in the block's visible text —
// `**bold**`, `*italic*`, `` `code` ``, `[text](href)` — because
// `ImGui::InputText` cannot mix fonts or colours within a single buffer.
// Code fences push the monospace font slot from `SmatchetImGuiFonts.h`;
// all other blocks use the default font.
//
// `ParseBlocks` is pure C++14 + md4c; the test rig links it without dragging
// in ImGui. `Render` is the ImGui-side wrapper called by the chat panel.

#ifndef SMATCHET_AI_CHAT_MARKDOWN_RENDER_H
#define SMATCHET_AI_CHAT_MARKDOWN_RENDER_H

#include <string>
#include <vector>

namespace AiChatMarkdownRender {

enum class BlockKind {
    Heading,   // # ... ###### — raw `# ` markers preserved at the start of Text
    Paragraph, // Default-font flowing paragraph
    CodeFence, // Fenced or indented code block; monospace
    ListItem,  // Single list-item line, prefixed `- ` (UL) or `<n>. ` (OL)
    Quote,     // Block-quote body, each line prefixed `> `
    Table,     // Raw table block, monospace (cells separated by ` | `)
    Other,     // Fallback (HR, raw HTML, defList, etc.)
};

struct Block {
    BlockKind Kind;
    std::string Text;    // Flattened content the InputTextMultiline displays
    std::string LangTag; // Non-empty only for CodeFence (md4c-reported lang info string)
    int HeadingLevel;    // 1..6 for Heading, 0 otherwise
    bool UseMonospace;   // CodeFence + Table = true; everything else = false

    Block() : Kind(BlockKind::Paragraph), HeadingLevel(0), UseMonospace(false) {}
};

// Pure-logic parse — runs md4c and returns one Block per top-level markdown
// block. Empty / whitespace-only input returns an empty vector. Tolerant of
// streaming-partial input (unclosed fences, unterminated paragraphs): md4c
// auto-closes at EOF, so the partial content surfaces in the last block.
std::vector<Block> ParseBlocks(const std::string& md);

// ImGui-side render. Walks `ParseBlocks(md)` and emits one
// ReadOnly InputTextMultiline per block. `scopeId` MUST be unique per call
// site (e.g. `"ai_msg_3"`) so widget IDs don't collide across messages.
// `panelWidth` is the current `ImGui::GetContentRegionAvail().x` — pre-fetched
// by the caller so the renderer can size paragraph heights without re-querying
// inside its own loop.
void Render(const std::string& md, const char* scopeId, float panelWidth);

} // namespace AiChatMarkdownRender

#endif // SMATCHET_AI_CHAT_MARKDOWN_RENDER_H
