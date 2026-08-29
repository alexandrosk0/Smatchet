#ifndef SMATCHET_SELECTABLE_TEXT_RUN_H
#define SMATCHET_SELECTABLE_TEXT_RUN_H

// SelectableTextRun — drag-selectable rich-rendered text overlay.
// ImGui ships no rich-text widget that supports drag-select + Ctrl+C — that
// constraint was the original reason `AiChatTextEditorView` put the
// AI chat inside a TextEditor instead of using `MarkdownPreviewRender`. This
// widget reverses that: it sits on top of any draw path that already emits
// `ImGui::TextUnformatted` (or `ImDrawList::AddText`) and adds the missing
// selection layer.
// Usage:
//   auto& ctx = SelectableText::Begin("##MyPreview");
//   ...then, for each styled run the caller has already drawn, record it via
//   SelectableText::RegisterSegment with the run's text bounds, screen
//   position, line height, font, width and optional hrefOpaque — see below.
//   SelectableText::EndBlock(ctx);  // optional — call after each markdown block
//                                    // so Ctrl+C inserts a newline between them.
//   SelectableText::End(ctx);       // services mouse + Ctrl+C, draws overlay.
// MVP scope:
//   - Mouse click + drag → contiguous range selection.
//   - Ctrl+C → copies the doc-order selected text to the clipboard.
//   - Block-boundary newlines via EndBlock().
//   - Hovered-href accessor for caller-driven link click routing.
// Deferred (slice-4 follow-ups; see docs/plans/shipped/markdown-language-definition-for-textedit.md
// § Deviations):
//   - Word-extend (double-click) + line/block-extend (triple-click).
//   - Keyboard selection (Shift+arrows, Shift+Home/End).
//   - perCharCumWidth caching keyed by (font, size, text-hash) — currently
//     recomputed every frame via `ImFont::CalcTextSizeA`. Acceptable at MVP
//     scale; promote when bucket-C perf shows it matters.
//   - Cross-window selection (the selection state is keyed per ImGui window-id
//     today; selecting across two separate windows is out of scope).

#include "imgui.h"

#include <string>
#include <vector>

struct ImFont;

namespace SelectableText {

// One rendered text segment. Constructed by `RegisterSegment`; one per styled-run call.
// The struct is exposed (not opaque) so the bucket-A test rig can construct
// synthetic contexts without an ImGui context.
struct Segment {
    int docOrder = 0;          // matches insertion order
    int blockId = 0;           // increments at EndBlock; selection inserts `\n` at boundaries
    ImVec2 pos = ImVec2(0, 0); // top-left screen position of the segment
    float lineH = 0.0f;        // text line height in pixels
    // Owned copy of the text bytes — the caller's StyledRun vector can
    // reallocate between frames; the segment outlives the source pointer.
    std::string textOwned;
    ImFont* font = nullptr;
    float fontSize = 0.0f;
    void* href = nullptr;
    float totalWidth = 0.0f;
};

struct Context {
    // Per-frame state — cleared by Begin().
    std::vector<Segment> segments;
    int nextDocOrder = 0;
    int currentBlockId = 0;

    // Persistent state — survives Begin / End for the same str-id.
    bool hasSelection = false;
    int selStartSeg = -1;
    int selStartChar = 0;
    int selEndSeg = -1;
    int selEndChar = 0;
    bool dragging = false;

    // Per-frame derived state set by End().
    int hoveredSeg = -1;
    int hoveredChar = 0;
};

// Begin / End bracket the selection-aware region. `strId` is hashed into an
// ImGuiID so selection state persists across frames for the same caller.
// Begin clears per-frame segment state but preserves the selection range.
Context& Begin(const char* strId);

// Optional block-boundary marker. Bumps `currentBlockId` so `GetSelectedText`
// can insert `\n` between segments crossing this boundary. Call after each
// markdown block (paragraph, heading, list item, code block, table row).
void EndBlock(Context&);

// Register a Segment for an already-rendered text run. Use when the caller is
// emitting glyphs through its own `ImGui::TextUnformatted` / `ImDrawList::AddText`
// path and just needs the selection bookkeeping (hit-test + overlay + Ctrl+C).
// `screenPos` is the top-left screen position of the run; `lineH` is the line
// height in pixels; `font` is the font used to render the text (so the
// hit-test can call `font->CalcTextSizeA` for sub-glyph offsets).
void RegisterSegment(Context& ctx, const char* begin, const char* end, ImVec2 screenPos, float lineH, ImFont* font,
                     float totalWidth, void* hrefOpaque = nullptr);

// Per-frame finaliser. MUST be called once per Begin. Services mouse input,
// draws the selection overlay, handles Ctrl+C.
void End(Context&);

// True when a non-empty selection exists AND its endpoints reference segments
// currently in the Context (so a stale selection from a previous frame's
// content no longer reports as valid).
bool HasSelection(const Context&);

// Concatenate the selected byte ranges in doc-order. Inserts `\n` at every
// EndBlock boundary that falls within the selection. Pure-logic — testable
// without an ImGui context.
std::string GetSelectedText(const Context&);

// Opaque href payload of the segment currently under the mouse, or nullptr.
// Caller (e.g. MarkdownPreviewRender) dispatches the click handler.
void* GetHoveredHref(const Context&);

} // namespace SelectableText

#endif // SMATCHET_SELECTABLE_TEXT_RUN_H
