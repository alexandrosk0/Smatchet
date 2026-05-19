# AI chat — TextEditor-backed selectable markdown render

## Goal

Replace the per-message `ImGui::InputTextMultiline` (current AI panel render after PR #289 commit `c6084855`) with **one** read-only `TextEditor` (ImGuiColorTextEdit) instance per panel. The whole conversation lives in that single widget. Result:

- Drag-select works **across messages**, not just within one.
- Markdown is **colored** in-place by an md4c-driven tokenizer: heading lines, fence open/close, inline code, bold/italic markers, links, list markers all get distinct palette colors.
- Text stays monospace throughout (matches the Lua editor exactly).
- Native cursor, Ctrl+C, Ctrl+A, word-jump, find — all inherited from TextEditor for free.

This is the only realistic Smatchet path to "rendered markdown that's selectable" because ImGui has no rich-text-with-selection widget and writing one from scratch is multi-week.

## Background

`TextEditor` is `Plugins/LuaConsole/ThirdParty/ImGuiColorTextEdit/TextEditor.{cpp,h}` (BalazsJako/ImGuiColorTextEdit, vendored). It already provides:

- Custom-drawlist glyph rendering with per-glyph `PaletteIndex` for color
- Anchor + cursor selection, mouse drag, double-click word, triple-click line
- Keyboard nav (arrows, home/end, page up/down, Ctrl+arrow word jump, shift-extend)
- Ctrl+C / Ctrl+A clipboard, palette switching, horizontal scroll
- `ReadOnly` mode that disables typing but keeps cursor + selection live
- `Colorize()` hook driven by a `LanguageDefinition` (regex-style token rules)

We need to add a "Markdown" language definition (or a manual colorizer pass) that classifies each glyph's `PaletteIndex` based on md4c block + span events, then drop the widget into `SmatchetAiAssistantUi.cpp` as the chat history view.

The plugin layering rule says `Source_Core/` cannot depend on `Plugins/`. To consume TextEditor from Source_Core, the widget must move to a Source-Core-or-shared location. We promote it to `Source_Core/ThirdParty/ImGuiColorTextEdit/` and rewire LuaConsole's include path.

## Critical files

| Path | Action |
|---|---|
| `Plugins/LuaConsole/ThirdParty/ImGuiColorTextEdit/TextEditor.{cpp,h}` | **Move** to `Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.{cpp,h}` |
| `Plugins/LuaConsole/LuaConsolePlugin.cpp` | Update include path to the new Source_Core location |
| `Plugins/LuaConsole/CMakeLists.txt` | Drop the third-party widget source listing; rely on Source_Core's link |
| `CMakeLists.txt` (root) | Add the moved TU to `CORE_SOURCES`; add include dir |
| `Source_Core/include/AiChatTextEditorRender.h` (new) | Public API `AiChatTextEditorView` wrapping a `TextEditor` + tokenizer + `RebuildBuffer(history, streamBuf, inFlight)` + `Draw(availW, availH)` |
| `Source_Core/src/AiChatTextEditorRender.cpp` (new) | Conversation serialiser + md4c-driven `RecolorBuffer()` + palette setup |
| `Source_Core/include/AiChatMarkdownTokens.h` (new) | Pure-logic tokenizer header — `Tokenize(md)` returns `std::vector<TokenSpan>` with line + col offsets |
| `Source_Core/src/AiChatMarkdownTokens.cpp` (new) | md4c walker; no ImGui / TextEditor deps |
| `Source_Core/src/SmatchetAiAssistantUi.cpp::DrawHistoryArea` | Replace per-message InputTextMultiline loop with one call to the panel-owned view's `Draw(availW, bodyH)` |
| `Source_Core/include/SmatchetUiSession.h` (or AppController owner) | Add `std::unique_ptr<AiChatTextEditorView>` member, lazily constructed on first draw |
| `tests/Source_Core/AiChatMarkdownTokens.test.cpp` (new) | Bucket-A doctest cases for the tokenizer |
| `tests/CMakeLists.txt` | Add the pure-logic source + the test file to the doctest source list |

## Interface contracts

### Tokenizer

```cpp
// Source_Core/include/AiChatMarkdownTokens.h
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
    int line;      // 0-based
    int colStart;  // byte offset within line
    int colEnd;    // exclusive
    TokenKind kind;
};

std::vector<TokenSpan> Tokenize(const std::string& md);

} // namespace AiChatMarkdownTokens
```

`Tokenize` runs md4c with block + span callbacks, tracking absolute byte offsets through the input. Block events (heading, fence) cover whole line ranges. Inline span events cover marker pairs (`**...**`, backtick pairs, `[...]`). Pure-logic so the doctest rig links it without ImGui or TextEditor.

### View

```cpp
// Source_Core/include/AiChatTextEditorRender.h
#include "AiTypes.h"
#include <memory>
#include <string>
#include <vector>
class TextEditor;

class AiChatTextEditorView {
  public:
    AiChatTextEditorView();
    ~AiChatTextEditorView();
    AiChatTextEditorView(const AiChatTextEditorView&) = delete;
    AiChatTextEditorView& operator=(const AiChatTextEditorView&) = delete;

    void RebuildBuffer(const std::vector<AiMessage>& history,
                       const std::string& streamingBuf,
                       bool inFlight);

    void Draw(float availW, float availH);

  private:
    std::unique_ptr<TextEditor> editor_;
    std::string lastSerialised_;
    bool wasAtTail_ = true;
};
```

### Conversation serialisation

Concatenate messages into one buffer with stable role prefixes the tokenizer recognises and treats as block-quote-like markers:

```
> You:
[user prompt body — raw markdown]

> Assistant:
[assistant reply body — raw markdown]

> You:
[next user turn]
```

The leading `> Role:` line is tokenised as `QuoteMarker` (bright color, bold-feel) so role transitions stand out. Bodies pass through normal markdown tokenisation. Streaming buffer appends as a trailing `> Assistant (streaming...):` block whose body is the in-flight `assistantStreamBuf`.

### Recolorize pass

After `editor_->SetText(serialised)`, walk `Tokenize(serialised)` and call `editor_->SetTokenColor(line, colStart, colEnd, paletteIdx)` (or the closest TextEditor API). Palette index choices:

| TokenKind | TextEditor PaletteIndex |
|---|---|
| Plain | `Default` |
| HeadingMarker / HeadingText | `Keyword` (bright) |
| FenceMarker / CodeFenceBody | `String` |
| InlineCode | `Number` |
| BoldMarker / BoldText | `Identifier` (slightly highlighted) |
| ItalicMarker / ItalicText | `Comment` (dimmed) |
| LinkText | `Function` (cyan) |
| LinkUrl | `Comment` |
| ListMarker | `Punctuation` |
| QuoteMarker | `Keyword` (role headers stand out) |
| Strike | `Preprocessor` (dimmed) |

Exact `PaletteIndex` names per `TextEditor.h`. If TextEditor doesn't expose a per-range color API directly, fall back to writing into `mLines[line][col].mColorIndex` after `SetText()` — that field exists in the public `TextEditor::Glyph` struct.

## Slice plan

Single squash commit on `claude/amazing-hamilton-3e1618`, on top of `c6084855`.

| Step | Files | Notes |
|---|---|---|
| 1. Plan-doc commit | `docs/design/ai-chat-textedit-markdown.md` | Prefix `wip(plan): ai-chat-textedit-markdown` |
| 2. Promote TextEditor | move TextEditor.{cpp,h}; update LuaConsole include + CMake; root CMake adds the TU + include dir | Verify Lua editor still builds + runs after the move |
| 3. Tokenizer | `AiChatMarkdownTokens.{h,cpp}` + `AiChatMarkdownTokens.test.cpp` | Pure-logic, md4c-only |
| 4. View | `AiChatTextEditorRender.{h,cpp}` | Owns the TextEditor instance, ReadOnly true, palette setup, RebuildBuffer + Draw + recolor pass |
| 5. Wire UI | `SmatchetAiAssistantUi.cpp::DrawHistoryArea` + owner ptr in session | Replace InputTextMultiline loop with one Draw call |
| 6. Build + test | dual-target build + ctest | Standalone clean; Lua editor still works; new tokens tests pass |
| 7. Commit + push + PR update | `feat(ai): TextEditor-backed selectable markdown chat` | PR #289 picks up the squash |

## Edge cases

- **Streaming**: `RebuildBuffer` runs on every frame the streaming buffer changes (cheap sha-equality short-circuit). Re-tokenise only the tail diff if the perf cost shows up.
- **Empty conversation**: `Draw` returns early; show a placeholder `TextDisabled("No messages yet.")` above the (empty) editor or just skip rendering.
- **Cross-message selection**: works for free because it's one widget. Verify by selecting from a heading in one message through a code block in the next + Ctrl+C; clipboard should contain both bytes with the role separator lines preserved.
- **Auto-scroll**: TextEditor exposes `MoveBottom()` / `SetCursorPosition`. Call `MoveBottom()` only when `wasAtTail_` is true (UI thread tracks this, mirroring the previous InputTextMultiline behaviour).
- **Huge messages**: TextEditor stores text as `std::vector<Line>` where each Line is a `std::vector<Glyph>`. 4 MiB stream cap → bounded. If a single line is enormously long, TextEditor's horizontal scrollbar handles it.
- **Unicode**: TextEditor handles UTF-8 internally. md4c is UTF-8-safe. Pass bytes through unchanged.

## Risks

- **R1 — TextEditor API**: it has no public `SetTokenColor(line, colStart, colEnd, idx)` method by default. Either patch the third-party file to expose one, or write the Glyph color directly via friend / wrapper. Decide at step 4 — keep the patch minimal so an upstream sync is feasible.
- **R2 — TextEditor ReadOnly cursor feel**: the cursor still blinks in ReadOnly mode; some users expect a pure "viewer" feel. Mitigation: disable cursor draw via palette `CursorColor = transparent` or patch `Render()` to skip cursor when `ReadOnly`.
- **R3 — Move regression**: relocating the TextEditor source under `Source_Core/` could break the Lua editor build if includes / link order change. Verify Lua editor opens + colorises Lua + accepts input post-move before continuing.
- **R4 — Cross-message selection bytes vs glyphs**: TextEditor returns selected text as a UTF-8 string from its line/glyph store. Verify Ctrl+C output matches the source bytes (role separator lines + body) byte-for-byte.
- **R5 — Perf on every-frame Rebuild**: serialising the conversation + re-tokenising on every UI frame would be wasteful. Mitigation: sha-equality short-circuit; only rebuild when `lastSerialised_ != newSerialised_`. Tokenisation runs after a rebuild only (not per-frame).
- **R6 — Dual-target build**: TextEditor is ImGui-only — fine for both Standalone and DX12 (DX12 also pulls ImGui via the existing setup). Verify the moved TU compiles into `SmatchetCore_DX12` (or is excluded if AI panel isn't enabled there).

## Verification

| Bucket | Coverage |
|---|---|
| A — doctest | Tokenizer covers: empty input, single heading, paragraph with bold + italic + inline code, fenced code with lang tag, bullet + ordered list, blockquote, mixed sequence, link, horizontal rule, streaming-partial unclosed fence, multi-line bold spanning a softbreak. |
| B — manual | Run `Smatchet.exe`; ask DeepSeek a multi-block prompt; verify (a) cursor blinks + can be placed anywhere via mouse; (b) drag-select within and across messages; (c) Ctrl+C pastes byte-identical content; (d) heading / bold / code coloured distinctly; (e) Lua editor still works the same as before the move. |
| D — sanitizer | Standard `ninja-test-msys2` ctest under ASan/UBSan. |
| E — ImGui Test Engine | Deferred to a backlog entry under `docs/backlog/agent-self-improvement/tooling.md` — the existing E bucket isn't wired yet. |

## Open questions

- **Q1 — Palette source**: reuse `TextEditor::GetDarkPalette()` colours, or define a chat-specific palette tuned for markdown contrast on Smatchet's default dark theme? **Default: reuse dark palette** for v1, tune later.
- **Q2 — Auto-scroll policy on user scroll-up**: keep the existing `wasAtTail_` behaviour (only auto-scroll when the user is already at the tail), same as the current InputTextMultiline path? **Default: yes, mirror existing behaviour.**
- **Q3 — Cursor on ReadOnly**: hide the blinking cursor for a more "viewer" feel, or keep it visible to make selection start-point obvious? **Default: hide cursor** in ReadOnly mode for chat panels.
- **Q4 — Role separator visual**: leading `> Role:` line, blank line between turns, or a horizontal rule? **Default: `> Role:` + blank line** so it tokenises as a quote and stands out.

## Implementation log

(populated on slice completion)

## Deviations from plan

(populated on slice completion)

## Verification

(populated on slice completion)
