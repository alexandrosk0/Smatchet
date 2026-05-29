# AI chat — TextEditor-backed selectable markdown render

## Goal

Replace the per-message `ImGui::InputTextMultiline` (current AI panel render after PR #289 commit `c6084855`) with **one** read-only `TextEditor` (ImGuiColorTextEdit) instance per panel. The whole conversation lives in that single widget. Result:

- Drag-select works **across messages**, not just within one.
- Markdown is **colored** in-place by an md4c-driven tokenizer: heading lines, fence open/close, inline code, bold/italic markers, links, list markers all get distinct palette colors.
- Text stays monospace throughout (matches the Lua editor exactly).
- Native cursor, Ctrl+C, Ctrl+A, word-jump, find — all inherited from TextEditor for free.

This is the only realistic Smatchet path to "rendered markdown that's selectable" because ImGui has no rich-text-with-selection widget and writing one from scratch is multi-week.

## Audit findings (BLOCKERS resolved below)

A fact-check against the actual TextEditor + md4c APIs surfaced two architectural blockers in the original plan; both are addressed in the revised design.

- **BLOCKER 1 — TextEditor has no public per-glyph color API.** `TextEditor::mLines` is private (`TextEditor.h:353`); the `Glyph::mColorIndex` field is public but unreachable from outside the class without an in-class accessor. The plan's "write into `mLines[line][col].mColorIndex` after `SetText()`" was not viable.
  - **Fix**: patch the vendored `TextEditor.{cpp,h}` to add one new public method `void SetTokenColor(int line, int colStart, int colEnd, PaletteIndex idx)` that range-writes the field. ~10 LOC patch in `TextEditor.cpp` plus a header declaration. Also add `void DisableColorizerPasses()` that sets `mColorizerEnabled = false` so our explicit colors aren't overwritten by `ColorizeInternal()`. Keep the patch minimal so upstream merges stay easy.

- **BLOCKER 2 — md4c callbacks do not expose source byte offsets.** `enter_block` / `leave_block` / `enter_span` / `leave_span` receive no offset info (md4c.h:363-367); `text()` only has `(MD_TEXTTYPE, const MD_CHAR* text, MD_SIZE size, void*)` (md4c.h:369). The plan's "md4c walker tracking byte offsets through the input" doesn't work — md4c hands us **parsed** text bytes, not source offsets.
  - **Fix**: drop md4c entirely for this tokenizer. Markdown's syntax is line-based + regex-tractable. Write a **hand-rolled line scanner** with three pieces:
    1. Line classification (heading prefix `#{1,6} `, fence boundary ` ``` `, list `^[-*+] ` / `^\d+\. `, quote `^> `, plain).
    2. State machine for fence-open / fence-close (multi-line, fence body suppresses inline scan).
    3. Inline regex pass per non-fence line: `` ` ``…`` ` ``, `**…**`, `*…*` (with negative lookaround), `[…](…)`, `~~…~~`.
  - Pure-logic, no md4c link in the test rig, more predictable behaviour, ~150 LOC. md4c stays available for `MarkdownPreviewRender` (its other consumer); we just don't use it here.

Other audit results (all OK as written):
- `TextEditor::SetReadOnly(bool)` at `TextEditor.h:211`; selection + Ctrl+C + Ctrl+A all work in ReadOnly.
- `TextEditor::MoveBottom()` at `TextEditor.h:245` for tail-scroll.
- `TextEditor::GetSelectedText()` concatenates lines with `\n` (TextEditor.cpp:2116-2118 → GetText line 106), so cross-message clipboard preserves separators.
- Nested `BeginChild` inside the panel's `##AiAssistantHistory` child is fine; set `mIgnoreImGuiChild = true` on the editor instance so we keep the parent child's scroll authority.
- LuaConsole already uses `TextEditor::LanguageDefinition::Lua()` (LuaConsolePlugin.cpp:118,182) — no impact from the move; our Markdown view skips `LanguageDefinition` entirely (we drive colors via `SetTokenColor`).
- Both `SmatchetPlugin_LuaConsole` (Standalone) and `SmatchetPlugin_LuaConsole_DX12` build TextEditor today (CMakeLists.txt:869,912). After the move, TextEditor links into `SmatchetCore` once and LuaConsole drops its copy.

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
| `Source_Core/src/AiChatMarkdownTokens.cpp` (new) | **Hand-rolled** line + regex scanner. No md4c link. No ImGui / TextEditor deps. |
| `Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.{cpp,h}` | **Patch** to add public `SetTokenColor(line, colStart, colEnd, PaletteIndex)` and `DisableColorizerPasses()` methods. ~10 LOC. |
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

`Tokenize` is a hand-rolled line + regex scanner (md4c does not expose source byte offsets so we cannot use it here). Algorithm:

1. Split input on `\n` into lines tracked with absolute line index.
2. Per-line state machine: track `inFence` (set when a line equals `` ``` `` or `` ```<lang> ``; cleared on the matching close).
3. Line classification while NOT in fence:
   - Starts with `#{1,6} ` → `HeadingMarker` (the leading `#`s + space) + `HeadingText` (rest of line).
   - Starts with `> ` → `QuoteMarker` (those 2 chars) then continue inline scan on the rest.
   - Starts with `[-*+] ` or `\d+\. ` → `ListMarker` (the marker) then continue inline scan on the rest.
   - Plain otherwise.
4. Line classification while IN fence: emit `CodeFenceBody` for the full line.
5. Fence boundary lines themselves emit `FenceMarker`.
6. Inline pass on each non-fence non-fence-boundary line:
   - Backtick pairs `` `…` `` → `InlineCode`.
   - `**…**` → `BoldMarker` + `BoldText` + `BoldMarker`.
   - `*…*` (with `(?<!\*)\*(?!\*)` negative lookarounds) → `ItalicMarker` + `ItalicText` + `ItalicMarker`.
   - `~~…~~` → `Strike`.
   - `\[([^\]]+)\]\(([^\)]+)\)` → `LinkText` for the `[...]` body + `LinkUrl` for the `(...)` body.
7. Output `TokenSpan{line, colStart, colEnd, kind}`. Plain gaps between spans are implied.

Pure-logic so the doctest rig links it without ImGui, TextEditor, or md4c. Inline regex pass uses `std::regex` from `<regex>` — C++11/14 safe.

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

After `editor_->SetText(serialised)`, immediately call `editor_->DisableColorizerPasses()` so the built-in `ColorizeInternal()` regex pass (which runs per-frame when `mColorizerEnabled=true`) doesn't overwrite our manual colors. Then walk `Tokenize(serialised)` and call the patched `editor_->SetTokenColor(line, colStart, colEnd, paletteIdx)` for each span. Palette index choices:

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

Exact `PaletteIndex` names per `TextEditor.h`. Fallback path (writing `mLines[line][col].mColorIndex` directly) is **NOT** available because `mLines` is private — the patched `SetTokenColor` is the only sanctioned write path.

## Slice plan

Single squash commit on `claude/amazing-hamilton-3e1618`, on top of `c6084855`.

| Step | Files | Notes |
|---|---|---|
| 1. Plan-doc commit | `docs/plans/shipped/ai-chat-textedit-markdown.md` | Prefix `wip(plan): ai-chat-textedit-markdown` |
| 2. Promote TextEditor + patch | move TextEditor.{cpp,h}; update LuaConsole include + CMake; root CMake adds the TU + include dir; **add `SetTokenColor` + `DisableColorizerPasses` public methods** (10 LOC patch) | Verify Lua editor still builds + runs after the move; patch must be additive (existing API unchanged) |
| 3. Tokenizer | `AiChatMarkdownTokens.{h,cpp}` + `AiChatMarkdownTokens.test.cpp` | Pure-logic, hand-rolled line + regex (no md4c) |
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

- **R1 — TextEditor patch surface**: we add two public methods. Patch must be minimal + clearly delimited (e.g. `// Smatchet — added for AI chat panel` comment markers around the additions) so a future upstream sync is mechanical. The patch is the only modification to a vendored third-party file in this slice.
- **R2 — TextEditor ReadOnly cursor feel**: the cursor still blinks in ReadOnly mode; some users expect a pure "viewer" feel. Mitigation: disable cursor draw via palette `CursorColor = transparent` or patch `Render()` to skip cursor when `ReadOnly`.
- **R3 — Move regression**: relocating the TextEditor source under `Source_Core/` could break the Lua editor build if includes / link order change. Verify Lua editor opens + colorises Lua + accepts input post-move before continuing.
- **R4 — Cross-message selection bytes vs glyphs**: TextEditor returns selected text as a UTF-8 string from its line/glyph store. Verify Ctrl+C output matches the source bytes (role separator lines + body) byte-for-byte.
- **R5 — Perf on every-frame Rebuild**: serialising the conversation + re-tokenising on every UI frame would be wasteful. Mitigation: sha-equality short-circuit; only rebuild when `lastSerialised_ != newSerialised_`. Tokenisation runs after a rebuild only (not per-frame). `DisableColorizerPasses()` must be called after every `SetText()` because `ColorizeInternal()` runs per-frame when `mColorizerEnabled=true` and would otherwise overwrite our spans.
- **R6 — Dual-target build**: TextEditor is ImGui-only — fine for both Standalone and DX12 (DX12 already builds it today via `SmatchetPlugin_LuaConsole_DX12` per CMakeLists.txt:912). After the move, the DX12 target compiles TextEditor as part of `SmatchetCore_DX12` and LuaConsole's plugin lib drops its copy. Verify both branches link.

- **R7 — Nested BeginChild interaction**: AI panel renders inside `BeginChild("##AiAssistantHistory", ...)`; TextEditor opens its own BeginChild during render unless `mIgnoreImGuiChild = true`. Set the flag at view construction so the parent child keeps scroll authority and the auto-scroll-to-tail logic continues to work.

## Verification

| Bucket | Coverage |
|---|---|
| A — doctest | Tokenizer covers: empty input, single heading, paragraph with bold + italic + inline code, fenced code with lang tag (verify fence state machine), bullet + ordered list, blockquote, mixed sequence, link, horizontal rule, streaming-partial **unclosed** fence (fence-open persists to EOF, body bytes coloured as code), italic-vs-bold disambiguation (`*x*` vs `**x**` vs `***x***`), backtick pairs not greedy. |
| B — manual | Run `Smatchet.exe`; ask DeepSeek a multi-block prompt; verify (a) cursor blinks + can be placed anywhere via mouse; (b) drag-select within and across messages; (c) Ctrl+C pastes byte-identical content; (d) heading / bold / code coloured distinctly; (e) Lua editor still works the same as before the move. |
| D — sanitizer | Standard `ninja-test-msvc` ctest under ASan/UBSan. |
| E — ImGui Test Engine | Deferred to a backlog entry under `docs/backlog/agent-self-improvement/tooling.md` — the existing E bucket isn't wired yet. |

## Open questions

- **Q1 — Palette source**: reuse `TextEditor::GetDarkPalette()` colours, or define a chat-specific palette tuned for markdown contrast on Smatchet's default dark theme? **Default: reuse dark palette** for v1, tune later.
- **Q2 — Auto-scroll policy on user scroll-up**: keep the existing `wasAtTail_` behaviour (only auto-scroll when the user is already at the tail), same as the current InputTextMultiline path? **Default: yes, mirror existing behaviour.**
- **Q3 — Cursor on ReadOnly**: hide the blinking cursor for a more "viewer" feel, or keep it visible to make selection start-point obvious? **Default: hide cursor** in ReadOnly mode for chat panels.
- **Q4 — Role separator visual**: leading `> Role:` line, blank line between turns, or a horizontal rule? **Default: `> Role:` + blank line** so it tokenises as a quote and stands out.

## Implementation log

- `feat(ai): selectable monospace markdown chat via TextEditor` (commit on `claude/amazing-hamilton-3e1618`):
  - Moved `Plugins/LuaConsole/ThirdParty/ImGuiColorTextEdit/TextEditor.{cpp,h}` to `Source_Core/ThirdParty/ImGuiColorTextEdit/`; SmatchetCore now compiles the TU once and exposes it to both the standalone Lua editor and the new AI chat view.
  - Updated `CMakeLists.txt` so the TU appends to `CORE_SOURCES`, plus put the header dir on `SmatchetCoreInterface` (`PUBLIC`). Dropped the per-plugin source entries from `SmatchetPlugin_LuaConsole` + `SmatchetPlugin_LuaConsole_DX12`.
  - Patched the vendored TextEditor with two additive public methods marked `// Smatchet — added for AI chat panel`: `SetTokenColor(line, colStart, colEnd, PaletteIndex)` (in `.cpp`) and `DisableColorizerPasses()` (inline in `.h`).
  - New `Source_Core/include/AiChatMarkdownTokens.h` + `Source_Core/src/AiChatMarkdownTokens.cpp`: pure-logic line + std::regex scanner. Emits `(line, colStart, colEnd, kind)` spans for heading / fence / inline code / bold / italic / link / list / quote / strike. No md4c / ImGui / TextEditor dependency.
  - New `Source_Core/include/AiChatTextEditorRender.h` + `Source_Core/src/AiChatTextEditorRender.cpp`: `AiChatTextEditorView` owns one `TextEditor` instance per panel. Serialises conversation with `> You:` / `> Assistant:` role prefixes; sha-equality short-circuit so per-frame rebuild is cheap; `MoveBottom()`-on-tail mirrors prior auto-scroll behaviour.
  - Replaced the per-message `ImGui::InputTextMultiline` loop in `SmatchetAiAssistantUi.cpp::DrawHistoryArea` with the new view. Dropped the `SmatchetImGuiFonts.h` include.
  - New tests at `tests/Source_Core/AiChatMarkdownTokens.test.cpp`: 17 doctest cases covering the documented edge list (empty / headings H1-H6 / H7 not heading / paragraph mix / fenced code with lang tag / unclosed fence / lists / blockquote / link / strike / mixed sequence / bold-vs-italic / role prefix / non-greedy backticks). `tests/CMakeLists.txt` picks up the new test + tokens TU.
- `docs(plan): ai-chat-textedit-markdown implementation log` — this revision commit.

## Deviations from plan

- **Palette mapping — `LinkText`**: plan says `PaletteIndex::Function`. That enumerator does not exist in `TextEditor::PaletteIndex`; substituted `KnownIdentifier` (cyan in the dark palette, same visual class).
- **Italic hand-roll**: emitted Italic spans directly from a manual scan + neighbouring-character guard (the plan flagged this as a likely deviation — `std::regex` lookbehind is unreliable on libstdc++/MinGW UCRT).
- **`mIgnoreImGuiChild` patch unnecessary**: TextEditor already exposes a public `SetImGuiChildIgnored(bool)` setter; no third additive method needed. Two patched methods, not three.
- **`AiChatTextEditorView` ownership**: plan suggested a `std::unique_ptr` member on `UiDrawSession`. Used a function-local `static` inside `DrawHistoryArea` instead — there is exactly one AI panel and this avoids touching the session struct. A migration to `UiDrawSession` is trivial if a second panel ever ships.
- **`RebuildBuffer` signature**: takes `wasAtTail` from the caller rather than tracking it internally. Keeps the view stateless w.r.t. ImGui scroll state, which the parent BeginChild owns.
- **Superseded by [`markdown-language-definition-for-textedit.md`](markdown-language-definition-for-textedit.md) § Slice 5 (2026-05-19)**. The original constraint that drove this design ("ImGui has no rich-text-with-selection widget and writing one from scratch is multi-week") was eventually addressed by Slice 4 of that follow-up plan — a `SelectableTextRun` overlay that adds drag-select + Ctrl+C on top of any ImGui draw path. Slice 5 then moved the AI chat off `AiChatTextEditorView` onto `MarkdownPreviewRender` + the new overlay, fully retiring the TextEditor-as-prose-view experiment that landed here. Files deleted in that slice: `AiChatTextEditorRender.{h,cpp}`, plus the `MarkdownChat()` LD + `SetTokenColor` / `DisableColorizerPasses` vendored-patch methods on `TextEditor`. The original `AiChatMarkdownTokens` scanner was already retired in Slice 1 (LD-based coloring).

## Verification

- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — **passed** (174/174 targets, no warnings in new files).
- `cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12` — new TUs (`AiChatMarkdownTokens.cpp`, `AiChatTextEditorRender.cpp`, vendored `TextEditor.cpp`) all compiled successfully under DX12. A pre-existing `WhisperAiAssistantAutosendScenario.cpp` failure (references `assistantPanelOpen` which is `#if defined(SMATCHET_WITH_AI)` gated; DX12 build does not define `SMATCHET_WITH_AI`) stopped the chain at 376/383 — unrelated to this slice, predates by commit `4c56b83b`.
- `cmake --build --preset ninja-test-msvc` — built; ran `SmatchetTests.exe --test-case="*AiChatMarkdownTokens*"` — **17/17 passed, 63/63 assertions**. Targeted AI regression `*Provider*,*DeepSeek*,*ModelSignature*,*AiClient*,*Model*` — **65/65 passed, 579/579 assertions**.
- Full `SmatchetTests.exe` — 776/783 passed; the 7 failures are pre-existing `AgentProposalStore` "unable to open database file" errors unrelated to this slice (concurrent worktree SQLite file collisions).
- Bucket E (ImGui Test Engine) — rig is wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`); coverage of this slice's rendered colorizer per-glyph palette stays deferred to a future targeted test pass, no live backlog entry yet.
