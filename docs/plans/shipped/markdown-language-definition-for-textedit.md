# Extend ImGuiColorTextEdit — `LanguageDefinition::Markdown()` + 3 consumers
<!-- plan-date: 2026-05-19 -->

## Context

Today, markdown in `TextEditor` (vendored `Source_Core/ThirdParty/ImGuiColorTextEdit/`) is highlighted only inside the AI chat panel via the **hand-rolled** `AiChatMarkdownTokens` scanner (PR #289, plan [`ai-chat-textedit-markdown.md`](ai-chat-textedit-markdown.md)). That scanner runs a state machine + `std::regex` pass and writes per-glyph palette indices through the patched `TextEditor::SetTokenColor`. The built-in `ColorizeInternal()` is force-disabled via `DisableColorizerPasses()` to keep it from clobbering manual spans.

That works for AI chat but isn't reusable: any other markdown surface (plan-doc viewer, ticket description editor, markdown preview edit-mode) has to re-implement coloring, or wire `AiChatMarkdownTokens` even though its enums (`UserRoleMarker` / `AssistantRoleMarker` / `UserMsgBody`) are chat-specific.

[Issue #155 on BalazsJako/ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit/issues/155) gives a different mechanism — a `LanguageDefinition::Markdown()` that plugs into TextEditor's **native** regex pipeline. Every existing built-in (`Lua()`, `CPlusPlus()`, etc.) ships as a `LanguageDefinition`; `SetLanguageDefinition()` is the documented seam. A markdown LD makes coloring available to **every** TextEditor instance with one line.

**User decisions** (2026-05-19):
- Outcome: **add LD + migrate AiChat** to it (single colorization mechanism).
- Surfaces: ship the LD, then wire plan-doc viewer + ticket description editor + markdown preview edit-mode.
- **Then**: build a custom `SelectableTextRun` widget so the description preview (and AI chat) become rich-rendered AND drag-selectable. Resolves the "no rich-text-with-selection widget in ImGui" constraint that originally pushed AiChat onto TextEditor in PR #289.

Outcome: one canonical markdown highlighter, three coloured-edit surfaces (plan-doc viewer, ticket description editor, AiChat in Slice 1 — later retired in Slice 5), plus a new `SelectableTextRun` ImGui-level widget powering rich-rendered selectable previews everywhere md4c rendering already runs.

## Architecture decision: LD regex pipeline vs hand-rolled state machine

The native LD pipeline (`ColorizeInternal()` in [`Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp:2011-2087`](../../Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp)) is **per-line regex** plus **one** multi-line block-comment span driven by `mCommentStart` / `mCommentEnd`. The AiChat hand-rolled scanner is a **multi-state** line classifier (fence open / fence body / fence close / non-fence with inline pass).

**Same-token block-comment bug (must fix)**: setting `mCommentStart == mCommentEnd == "\`\`\`"` does NOT work out of the box. `ColorizeInternal` opens the span at the first ` ``` ` (line 2042-2046), then the end-string check on line 2055-2058 (`equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred)`) immediately matches the **same** 3 bytes on the very next iteration and closes the span. The pipeline implicitly assumes `mCommentStart != mCommentEnd` (`/* */`, `--[[ ]]`, `<!-- -->` — every built-in LD respects this).

Issue #155 sidesteps the bug by using `<!--` / `-->`, accepting that multi-line fences are not coloured beyond what a single-line regex (`` ```.*``` ``) captures.

Tradeoff for fenced code blocks:

| Mechanism | Multi-line fence body | HTML comment | Code |
|---|---|---|---|
| Hand-rolled (status quo) | Correct | Lost (not wired) | ~150 LOC + state |
| LD with `mCommentStart="\`\`\`"` / `mCommentEnd="\`\`\`"` — naive | **Broken** — self-closes after 3 bytes | Lost | ~80 LOC LD, but unusable as-is |
| LD with `mCommentStart="\`\`\`"` / `mCommentEnd="\`\`\`"` + tiny `ColorizeInternal` guard patch | Correct (re-uses comment span) | **Lost** (only one span slot) | ~80 LOC LD + ~6 LOC `ColorizeInternal` guard |
| LD with `mCommentStart="<!--"` / `mCommentEnd="-->"` (issue #155) | Wrong — single-line regex only | Correct | ~80 LOC, regex only |

**Decision**: ship LD with fence-as-comment-span (`mCommentStart="\`\`\`"`) AND a small additive guard in `ColorizeInternal` that makes same-token block-comment delimiters work correctly (~6 LOC, slice 1). Fenced code is the dominant multi-line construct in our docs and AI chat output; HTML comments are negligible in our markdown corpus. The Issue #155 single-line `<!--` wiring is documented in the LD source comments so future code can flip the trade.

**`ColorizeInternal` guard patch** (additive, no behaviour change for existing LDs because they all use distinct start/end strings):

```cpp
// Skip end-string check when start==end and we haven't moved past the opening token —
// otherwise same-token block delimiters (e.g. markdown ``` fences) self-close on the same bytes.
const bool sameTokenDelim = (mLanguageDefinition.mCommentStart == mLanguageDefinition.mCommentEnd);
const bool insideOpener = (commentStartLine == currentLine &&
                           currentIndex < commentStartIndex + (int)startStr.size());
if (!(sameTokenDelim && insideOpener)) {
    // existing end-string match block at TextEditor.cpp:2054-2059
}
```

Marked with `// Smatchet — added for markdown LD` delimiters per the existing patch convention.

AiChat's chat-specific role markers (`> You:` / `> Assistant:`) move to a thin **wrapper LD** — `MarkdownChat()` — that clones `Markdown()` and prepends a role-line regex. Both LDs live in the same TU.

## Critical files

| Path | Action |
|---|---|
| `Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.h` | Add `static const LanguageDefinition& Markdown();` and `static const LanguageDefinition& MarkdownChat();` declarations next to the existing `Lua()` family. ~2 LOC. |
| `Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp` | (a) Implement both LDs. `Markdown()` per the regex table below; `MarkdownChat()` reuses `Markdown()` and adds role-line regex. ~80 LOC, additive. (b) Apply the same-token block-delimiter guard patch in `ColorizeInternal()` at TextEditor.cpp:2054 (see decision section). ~6 LOC. Mark with `// Smatchet — added for markdown LD` delimiters per the existing patch convention. |
| `Source_Core/include/AiChatTextEditorRender.h` | Drop the implementation-detail include of `AiChatMarkdownTokens.h`. Public API unchanged. |
| `Source_Core/src/AiChatTextEditorRender.cpp` | Replace `Tokenize()` + `SetTokenColor()` loop with `editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::MarkdownChat()); editor_->SetColorizerEnable(true);`. Drop the `DisableColorizerPasses()` call. Conversation serialiser (the `> You:` / `> Assistant:` prefix scheme) **stays** — the wrapper LD's role-regex matches that exact prefix shape. |
| `Source_Core/include/AiChatMarkdownTokens.h` | **Delete** (no other callers — confirmed via grep on `AiChatMarkdownTokens` and `Tokenize`). |
| `Source_Core/src/AiChatMarkdownTokens.cpp` | **Delete**. |
| `tests/Source_Core/AiChatMarkdownTokens.test.cpp` | **Delete** the chat-tokens cases; replace with bucket-A doctest on the LD's regex strings (compile each regex once, assert match shape on representative inputs). The colorizer's per-glyph palette is integration territory (bucket E — rig wired but no test landed for this slice yet). |
| `tests/CMakeLists.txt` | Drop the `AiChatMarkdownTokens.cpp` source + `.test.cpp` entries; add `MarkdownLanguageDefinition.test.cpp`. |
| `CMakeLists.txt` (root) | Remove `AiChatMarkdownTokens.cpp` from `CORE_SOURCES`. |
| `Source_Core/src/TicketFieldEditor.cpp` (long-text editor modal) | Replace the description-edit-mode `ImGui::InputTextMultiline` (line 1340, `"##LongTextEditorBuf"`) with a `TextEditor` instance configured with `Markdown()` LD. Keep the existing preview-mode `MarkdownPreviewRender` path (md4c-based rich render — different surface from raw-source coloring). Toggle between the two via the existing modal toggle. |
| `Source_Core/include/SmatchetPlanDocViewerUi.h` (new) | Public `DrawPlanDocViewer(UiDrawSession&)` declaration + toggle flag added to `UiDrawSession` (`showPlanDocViewer`). |
| `Source_Core/src/SmatchetPlanDocViewerUi.cpp` (new) | New read-only `TextEditor` viewer for `docs/plans/active/*.md` and `docs/adr/*.md`. File picker combo (sorted alpha) + `TextEditor` set to `ReadOnly` + `Markdown()` LD + `SetShowLineNumbers(false)`. Reads from disk via `ghc::filesystem::directory_iterator` (or `std::ifstream` — match the codebase's prevailing pattern). |
| `Source_Core/src/Commands/ViewToggleCommands.cpp` | Register `view.toggle.plan_doc_viewer` matching the existing `view.toggle.<id>` naming convention (see `view.toggle.source_blame`, `view.toggle.performance`, etc.). |
| `Source_Core/src/MarkdownPreviewRender.cpp` (edit-mode toggle wiring) | Where consumers (long-text modal, future preview pane) toggle edit ↔ preview, the edit side now hosts a `TextEditor` with `Markdown()` LD. The `Render(md, opts)` preview function itself is untouched (md4c-driven; rich render). |

## `Markdown()` LanguageDefinition — regex table

Driven by [`TextEditor::LanguageDefinition::TokenRegexStrings`](../../Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.h). Per-line evaluation order matters — the colorizer takes the first match.

| Pattern | PaletteIndex | Markdown construct |
|---|---|---|
| `` ^#{1,6}[ \t].*$ `` | `Keyword` | ATX heading (full line) |
| `` ^>[ \t].*$ `` | `Comment` | Blockquote (full line) |
| `` ^[ \t]*([-*+]|\d+\.)[ \t] `` | `Punctuation` | List marker only |
| `` !?\[[^\]]+\]\([^)]+\) `` | `KnownIdentifier` | Link / image (whole token) |
| `` \*\*[^\*]+\*\* `` | `Identifier` | Bold |
| `` ~~[^~]+~~ `` | `Preprocessor` | Strikethrough |
| `` \*[^\*\n]+\* `` | `String` | Italic (after Bold, so `**…**` wins) |
| `` `[^`\n]+` `` | `Number` | Inline code |
| `` ^(-{3,}\|\*{3,}\|_{3,})$ `` | `Punctuation` | Horizontal rule |

Plus:
- `mCommentStart = "\`\`\`"`, `mCommentEnd = "\`\`\`"` → fenced code body coloured as `MultiLineComment` palette (multi-line correct via `ColorizeInternal()`'s comment-span pass + the new guard patch).
- `mSingleLineComment = ""` (markdown has none).
- `mAutoIndentation = false`.
- `mCaseSensitive = true`.
- `mPreprocChar = 0` (unused — set to a char no markdown line starts with).
- `mTokenize = nullptr` (regex-only).
- `mName = "Markdown"`.

Palette mapping was tuned for readability on the existing dark palette — headings + role markers as bright `Keyword`, code as `Number` / `MultiLineComment`, italic dimmed via `String`, bold via `Identifier`. Maps cleanly to the existing AiChat plan-doc palette table so visual regression is minimal.

## `MarkdownChat()` — chat wrapper LD

Clones `Markdown()` and adds:

| Pattern | PaletteIndex | Construct |
|---|---|---|
| `` ^>[ \t]+(You|Assistant)(\s*\(streaming\.\.\.\))?:.*$ `` | `Keyword` | Role line (`> You:` / `> Assistant:` / `> Assistant (streaming...):`) |

Higher precedence than the generic blockquote regex (insert before it in `mTokenRegexStrings`). Everything else inherits.

Existing AiChat conversation serialiser (the `> Role:` prefix scheme in `AiChatTextEditorRender.cpp`) **stays unchanged** — the wrapper LD's role-regex is designed to match that exact byte shape so the migration is wiring-only.

## Slice plan — five sequential PRs

Five slices. Slices 1-3 ship the LD + three coloured-edit surfaces. Slice 4 builds `SelectableTextRun` (the custom selection layer the AiChat plan flagged as "multi-week"). Slice 5 retires the TextEditor-as-prose-view experiment from PR #289 and moves AI chat onto the same rich-rendered selectable surface as Slice 4.

Per the `Build / ctest cadence` rule each slice runs build + `scripts/dev/test-all.sh` once at the slice boundary.

### Slice 1 — `feat(textedit): LanguageDefinition::Markdown() + MarkdownChat()`

1. Patch `TextEditor.h` + `TextEditor.cpp`:
   - Add `Markdown()` + `MarkdownChat()` static factories (additive).
   - Apply same-token block-delimiter guard in `ColorizeInternal()` at line ~2054 (additive ~6 LOC; no behaviour change for existing LDs).
2. New bucket-A test `tests/Source_Core/MarkdownLanguageDefinition.test.cpp` — compile each `mTokenRegexStrings` entry via `std::regex(...)`; assert match/non-match on representative inputs (heading w/ space pass, `#foo` no-space fail, `**bold**` matches Bold not Italic, ordered-list marker, role line for `MarkdownChat()`).
3. Migrate AiChat: drop `AiChatMarkdownTokens.{h,cpp}` + test, swap `AiChatTextEditorRender.cpp` to `SetLanguageDefinition(MarkdownChat())` + `SetColorizerEnable(true)`. Visual smoke against existing AI chat panel (Smatchet.exe → DeepSeek prompt).
4. **Patch survivors**: `SetTokenColor` + `DisableColorizerPasses` are retired by this slice (no more callers). Keep both for now (additive, harmless, useful escape hatch); document a follow-up cleanup ticket. `SetShowLineNumbers` stays (used by the new plan-doc viewer + ticket modal).

### Slice 2 — `feat(ui): plan-doc viewer (docs/plans/active/*.md, docs/adr/*.md)`

5. New `SmatchetPlanDocViewerUi.{h,cpp}` — file picker (combo over `docs/plans/active/*.md` + `docs/adr/*.md`, glob via `ghc::filesystem::directory_iterator`) + read-only `TextEditor` with `Markdown()` LD + `SetShowLineNumbers(false)` + `SetReadOnly(true)`. Lazy file-load on combo-change (single-buffer reuse).
6. Register `view.toggle.plan_doc_viewer` in `ViewToggleCommands.cpp` matching the existing snake_case pattern. Add `showPlanDocViewer` toggle to `UiDrawSession`. Bind a menu item under View → Plan docs.

### Slice 3 — `feat(ui): markdown-coloured edit mode for description fields`

7. `TicketFieldEditor.cpp` long-text editor modal at line 1340 (`ImGui::InputTextMultiline("##LongTextEditorBuf", s_ActiveLongTextState.Buffer.data(), ...)`): replace edit-mode `InputTextMultiline` with a function-local `static TextEditor` configured with `Markdown()` LD (writable, `SetShowLineNumbers(false)`, `SetImGuiChildIgnored(true)` + wrap in our own `BeginChild("##LongTextEditorBuf", ...)` so the existing scroll-sync `FindWindowByID(editorId)` keeps working). On modal open, `editor.SetText(std::string(s_ActiveLongTextState.Buffer.data()))`. On modal save, `auto text = editor.GetText(); std::strncpy(s_ActiveLongTextState.Buffer.data(), text.c_str(), kBufferSize - 1);` (preserves the existing 64 KiB buffer cap).
8. Preview-mode (md4c `MarkdownPreviewRender::Render`) untouched **in this slice**. The modal's edit/preview toggle now swaps `TextEditor` ↔ `MarkdownPreviewRender`. The `##WorklogDesc` `InputTextMultiline` at line 1168 stays as plain-text input — worklogs are short comments, not markdown.

### Slice 4 — `feat(ui): selectable rich-rendered markdown preview (SelectableTextRun)`

Goal: make every visible character in `MarkdownPreviewRender` drag-selectable + Ctrl+C-copyable while keeping the existing rich render (heading scaling, BoldItalic font, clickable links, rich tables, `CppSyntaxHighlight` inside code blocks).

Today `MarkdownPreviewRender` flushes each `StyledRun` via `ImGui::TextUnformatted` / styled font swaps. `TextUnformatted` registers its rect with ImGui but does not expose hit-testable per-char positions — selection is impossible without a layer underneath. Adding a custom drawer that uses the public ImGui primitives (`ItemAdd`, `ImFont::CalcTextSizeA`, `CalcWordWrapPositionA`, `ImDrawList::AddText` / `AddRectFilled`, `SetClipboardText`) gives a fully selectable rich-rendered surface without patching vendored ImGui.

**Implementation**:

9. New `Source_Core/include/SelectableTextRun.h` + `Source_Core/src/SelectableTextRun.cpp`:
   - Public API:
     ```cpp
     namespace SelectableText {
     struct Context;
     Context& Begin(const char* id);           // per-ImGui-window context, keyed by str-id.
     void TextRun(Context&, const char* begin, const char* end,
                  ImFont* font, ImU32 color,
                  float wrapWidth, void* hrefOpaque = nullptr);
     void End(Context&);                       // draws selection overlay, services input + Ctrl+C.
     bool HasSelection(const Context&);
     std::string GetSelectedText(const Context&);
     } // namespace SelectableText
     ```
   - `TextRun` measures with `ImFont::CalcTextSizeA(size, FLT_MAX, wrapWidth, begin, end, &remaining)` in a wrap-loop, calls `ImGui::ItemAdd` per wrap-segment, draws via `window->DrawList->AddText(font, size, pos, color, segBegin, segEnd)`, and stores `{docOrderIdx, font, pos, segBegin, segEnd, perCharCumWidth[]}` in the `Context` so hit-test + sub-range overlays are O(1) per segment.
   - Char-offset hit-test: `mouseX` minus segment x-origin → binary search into `perCharCumWidth` → char index. Same data used to compute sub-range pixel rects for the selection overlay (start-char x to end-char x).
   - `End()` runs the per-frame input + overlay pass:
     - On `ImGui::IsMouseClicked(0)` over any registered segment → `selStart = (docIdx, charOff)`.
     - On `ImGui::IsMouseDown(0)` → update `selEnd`.
     - Double-click → word-extend via per-segment `isspace` scan.
     - Triple-click → block-extend (segments sharing the same `blockId` — assigned by the caller per markdown block).
     - Shift+Home/End/arrows — out of scope for v1 (mouse-only).
     - `(IsKeyDown(LeftCtrl) || RightCtrl) && IsKeyPressed(C, false)` && `HasSelection` → build text via `GetSelectedText` (walk segments in doc-order range, append `[startChar..endChar]` slices, emit `\n` at block boundaries), call `ImGui::SetClipboardText`.
     - Selection overlay: walk segments in `[selStart, selEnd]`, emit `window->DrawList->AddRectFilled(subRect, selectionColor)` per segment-segment intersection (z-order: drawn before glyphs would clobber text — instead draw after with `selectionColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg)` which has alpha; pre-rect to background then overlay works either way).
   - Thread-safety: ImGui is single-thread UI; context held in a function-local `std::unordered_map<ImGuiID, Context>` keyed by window id.

10. `Source_Core/src/MarkdownPreviewRender.cpp` integration:
    - In the per-block flush helper, replace each `ImGui::TextUnformatted(run.text.c_str())` with `SelectableText::TextRun(ctx, run.text.data(), run.text.data() + run.text.size(), runFont, runColor, wrapWidth)`.
    - Bracket the whole preview render with `auto& ctx = SelectableText::Begin("##LongTextPreview"); ... SelectableText::End(ctx);`.
    - Code blocks (currently rendered through `CppSyntaxHighlight` inside `BeginChild`) get their own per-block `Begin/End` so child-window scroll offsets are honoured.
    - Table cells: each cell starts a sub-doc-order range; cross-cell selection works via the shared `Context`.
    - Heading scaling, clickable link visit-on-click, lossy round-trip warning — all unchanged.

11. New bucket-A test `tests/Source_Core/SelectableTextRun.test.cpp` — pure-logic coverage of the doc-order range walk + char-offset slicing (the hit-test math itself is pixel-based and gets bucket B / E coverage):
    - `GetSelectedText` against a synthetic context with manually-populated segments — start mid-segment-A, end mid-segment-C → expected concatenation.
    - Empty selection → empty string.
    - Reverse selection (selEnd < selStart by doc-order) → normalised forward.
    - Block-boundary newline insertion when consecutive segments cross blocks.

### Slice 5 — `feat(ai): rich-rendered selectable AI chat (retire TextEditor path)`

Goal: AI chat panel renders through the same rich-rendered selectable path as the description preview. Drops the TextEditor-as-prose-view experiment from PR #289 in favour of full rich markdown render with cross-message selection.

12. `Source_Core/src/SmatchetAiAssistantUi.cpp::DrawHistoryArea`: replace the `AiChatTextEditorView` call (added in PR #289) with per-message `MarkdownPreviewRender::Render` calls wrapped in a single outer `SelectableText::Begin("##AiAssistantHistory") / End` so drag-select crosses message boundaries. Each message contributes a `blockId` segment-range; the role-prefix line (`> You:` / `> Assistant:`) renders as a heading-like styled run with the role color (replaces the `MarkdownChat()` LD's role-line regex from Slice 1).
13. Delete `Source_Core/include/AiChatTextEditorRender.h` + `.cpp` + `AiChatTextEditorView` member from owning session. Drop the `> You:` / `> Assistant:` raw-source serialiser — role prefixes are now ImGui-rendered, not raw markdown.
14. Drop `MarkdownChat()` LD from `TextEditor.cpp` (its sole caller retires). `Markdown()` LD stays — still used by Slice 2 (plan-doc viewer) + Slice 3 (description edit mode).
15. Audit: `SetTokenColor` + `DisableColorizerPasses` patches on the vendored TextEditor have no callers after this slice. Strip them as part of the same commit (`Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.{h,cpp}` reverts those two methods). Reduces vendored-patch surface to one (`ColorizeInternal` same-token guard from Slice 1) plus `SetShowLineNumbers` (still used by Slices 2 + 3).
16. `tests/Source_Core/AiChatMarkdownTokens.test.cpp` — already deleted in Slice 1. No further test changes here; Slice 4's `SelectableTextRun` tests + `MarkdownLanguageDefinition` tests cover the new path.

**Slice-5 implication**: this slice supersedes PR #289's design choice ("TextEditor-backed selectable monospace markdown chat"). The original plan ([`ai-chat-textedit-markdown.md`](ai-chat-textedit-markdown.md)) was the right call given the then-current constraints (`ImGui has no rich-text-with-selection widget and writing one from scratch is multi-week`). Slice 4 IS that multi-week write — once it lands, the original constraint dissolves. Update [`ai-chat-textedit-markdown.md`](ai-chat-textedit-markdown.md) `## Deviations from plan` with a pointer to Slice 5 ("widget was eventually written; chat moved off TextEditor").

## Edge cases

- **Multi-line fence with language tag** (` ```cpp `): `ColorizeInternal()` enters the multi-line comment span at the opening fence (which matches `mCommentStart` regardless of trailing language tag — `mCommentStart` is matched as a prefix per TextEditor.cpp:2035) and exits at the closing fence. Verified via test on `` ```cpp\nint x = 1;\n``` ``.
- **Single backtick at end of line in fence body**: stays inside `MultiLineComment` palette (the inline-code regex is shadowed by the active comment span — `ColorizeInternal()` runs comment detection before regex).
- **Streaming chat tail**: the `> Assistant (streaming...):` role line gets coloured by the `MarkdownChat()` role regex on the same frame the streaming buffer rebuilds (the LD's `mColorizerEnabled=true` triggers per-frame colorizer; cheap because TextEditor only re-colorizes changed lines via `mColorRangeMin/Max`).
- **Italic-vs-bold**: `**foo**` matches Bold regex first (precedence in `mTokenRegexStrings` insertion order); `*foo*` falls through to Italic.
- **Long lines in plan docs**: TextEditor uses horizontal scroll; line numbers off (`SetShowLineNumbers(false)`) so the wrap-or-scroll trade-off is visible.
- **Ticket description with mixed ADF / markdown**: ADF is converted to markdown via `MarkdownConvert` before reaching the modal (existing TicketFieldEditor flow). TextEditor edit-mode receives plain markdown; round-trip happens at save (existing path).
- **Dual-target build**: `Markdown()` + `MarkdownChat()` are pure-data LD constructors with no ImGui drawing — both compile in `SmatchetStandalone` and `SmatchetCore_DX12`. Plan-doc viewer + ticket modal already exist in both targets; the slice's new code respects `Source_Core/include/` ↔ `Source_Core/src/` split.

## Verification

| Bucket | Coverage |
|---|---|
| A — doctest | (1) `tests/Source_Core/MarkdownLanguageDefinition.test.cpp` — compile each `mTokenRegexStrings` entry into `std::regex`; assert match against representative valid input + non-match against near-miss input (e.g. `#foo` (no space) must NOT match heading; `**foo*` must NOT match bold). (2) `tests/Source_Core/SelectableTextRun.test.cpp` — doc-order range walk + char-offset slicing + reverse-selection normalisation + block-boundary newline insertion. |
| B — manual | (1) AI chat: multi-block DeepSeek prompt; verify heading / bold / code coloured + drag-select across messages (Slice 1 with TextEditor; Slice 5 with rich render). (2) View → Plan docs: select `ai-chat-textedit-markdown.md`; verify headings + lists + fenced code + links coloured (Slice 2). (3) Jira ticket markdown description: edit mode coloured + save round-trip (Slice 3). (4) Description preview: drag-select across heading + paragraph + code block; Ctrl+C; paste into terminal; verify byte-identical content excluding visual markers (Slice 4). (5) AI chat with Slice 5: drag-select across messages with rich render; Ctrl+C; verify clickable links still work, heading scaling preserved, fenced code syntax-highlighted, drag-select crosses message boundaries. |
| C — perf | `perf-measure` against `scenario.run "AiAssistantBigPromptStream"` — per-frame steady-state ≤ 6.94 ms (Pillar 1). Two measurements: Slice 1 (TextEditor + LD) and Slice 5 (rich render + SelectableTextRun). Compare against the pre-PR-#289 baseline. SelectableTextRun adds a per-segment `CalcTextSizeA` measure pass — expected sub-ms for typical chat messages (< few hundred segments); profile + optimise if measurable. |
| D — sanitizer | `cmake --build --preset ninja-test-msvc` ctest under ASan/UBSan after each slice. |
| E — ImGui Test Engine | Per the existing `docs/backlog/agent-self-improvement/tooling.md` deferral. Drag-select replay + Ctrl+C clipboard assertion are the canonical bucket-E targets after Slice 4 lands — add an explicit entry under `tooling.md` when the rig is wired. |

## Risks

- **R1 — regex performance on large buffers**: `std::regex` is slow on libstdc++/MinGW UCRT; `ColorizeInternal()` runs all regexes per-line on each colorize pass. Mitigation: TextEditor already re-colorizes only changed line ranges (`mColorRangeMin/Max`); for the plan-doc viewer the document loads once. For the AiChat panel the stream-tail recolour is the only hot path — confirm via `perf-measure` (bucket C above). Fallback: keep `DisableColorizerPasses()` + the hand-rolled scanner alive in a `git revert` if perf regresses.
- **R2 — losing HTML comment coloring**: documented trade-off (decision section). Acceptable because fenced code wins on usage frequency in our corpus.
- **R3 — italic regex over-eager**: `\*[^\*\n]+\*` matches inside list lines starting with `*` if not anchored carefully. The list-marker regex captures the leading `* ` first (different precedence slot), but the italic regex on the line tail can still match a stray `*`. Mitigated by the `[^\*]` body class — unmatched markers won't span. Verify via doctest case.
- **R4 — `MarkdownChat()` regex order**: the role-line regex must precede the generic blockquote regex in `mTokenRegexStrings` (insertion order = match precedence). Slice 1 doctest asserts the wrapper LD's first regex is the role one.
- **R5 — TextEditor as editable widget**: `TextEditor` started life as a code editor; tab key inserts a tab, Enter inserts a newline, no auto-grow. Acceptable for ticket description editing (matches LuaConsole behaviour). Mitigation: document Ctrl+A / Ctrl+C / native edit shortcuts in the modal tooltip.
- **R6 — Plan-doc viewer scope creep**: a "select a doc and view it" widget is one combo + one TextEditor — keep it that. No favourites, no per-file recent list, no in-doc TOC in slice 2.
- **R7 — `SelectableTextRun` correctness on wrapped runs**: per-char pixel positions come from `CalcWordWrapPositionA` + `CalcTextSizeA`. Off-by-one in the binary search produces "selection lags one char behind cursor" — the most-common manual-selection-impl failure mode. Mitigation: tight bucket-A coverage on the slicing math (synthetic context with manually populated `perCharCumWidth`) + bucket-B verification on real prose during Slice 4 manual test pass. Fall-back: ship code-block-only selectability (option D from the discussion thread) if cross-block proves too fragile by Slice-4 end.
- **R8 — `SelectableTextRun` perf**: per-segment measure pass adds CPU per frame. Worst-case for big multi-MB ADF docs in the description preview. Mitigation: cache `perCharCumWidth` keyed by `(font, size, wrapWidth, textHash)`; rebuild only when any of those change. Big docs are already gated through the worker-thread `MarkdownConvert` path so the UI frame sees a steady stream of cached results.
- **R9 — Slice-5 retires PR #289 design**: the work that landed as PR #289 (TextEditor-based monospace selectable chat) becomes legacy. Mitigation: keep the original plan + implementation log intact in [`ai-chat-textedit-markdown.md`](ai-chat-textedit-markdown.md) for archeology — append a deviation pointing here. Don't `git revert` PR #289; let Slice 5's diff stand as the canonical migration.
- **R10 — link-href payload in `SelectableTextRun`**: `TextRun` takes a `void* hrefOpaque` so the caller can stash the link target. Clickable-link semantics (cursor change, hover tooltip, open-on-click) must be threaded through the new API without breaking the existing `clickableLinks` toggle in `MarkdownPreviewRender::Options`. Implementation detail: a per-segment hover-test in `End()` plus a public `SelectableText::GetHoveredHref(ctx) -> void*` accessor that the caller services with its own click handler (shell-open for full mode, no-op for tooltip mode).

## Out of scope

- Editor-mode bold/italic toolbar buttons, image-paste, drag-drop of attachments.
- Live preview side-by-side (could be a future iteration on slice 3 — though Slice 4 lifts the "selectable preview" constraint that pushed the user toward edit-mode coloring as a workaround).
- `IsCppLikeLangTag()` fenced-code C++ syntax dispatch — only applies to the md4c preview render's code-block path, not the LD edit-mode (it stays as-is and continues to work inside `SelectableTextRun`).
- Keyboard-driven selection in `SelectableTextRun` (Shift+arrows, Shift+Home/End, Ctrl+Shift+Right word-extend). Mouse-only for v1. Backlog entry under `tooling.md` once Slice 4 ships.
- Lifting `SelectableTextRun` into vendored ImGui as `ImGui::BeginSelectableText` / `TextRun` / `EndSelectableText`. Done as Smatchet-side helper in Slice 4; upstreaming is a separate effort once the API stabilises through 2-3 release cycles.

## Open questions (low-priority)

- **Q1 — Plan-doc viewer file scope**: `docs/plans/active/*.md` + `docs/adr/*.md` only, or all of `docs/**/*.md`? Default: design + ADR only (those are the curated long-form docs).
- **Q2 — Ticket description modal**: TextEditor as default edit widget, or opt-in toggle? Default: replace outright. Old `InputTextMultiline` path deleted.
- **Q3 — Strip `SetTokenColor` + `DisableColorizerPasses` patches after slice 1 lands**? Default: keep them (additive, harmless, useful escape hatch). Mark deprecation in a comment.

## Implementation log

- `34e9077` · Slice 1 — `LanguageDefinition::Markdown()` + `MarkdownChat()`; `ColorizeInternal()` same-token guard; AiChat migrated; `AiChatMarkdownTokens.{h,cpp,test.cpp}` deleted; bucket-A `MarkdownLanguageDefinition.test.cpp` added.
- `932b06d` · Slice 2 — `SmatchetPlanDocViewerUi.{h,cpp}` with file picker + read-only TextEditor; `view.toggle.plan_doc_viewer` registered; View menu → "Plan docs".
- `9361213` · Slice 3 — `TicketFieldEditor.cpp` long-text edit-mode swapped to `TextEditor` with `Markdown()` LD; bidirectional buffer sync handles dictation router + async seed paths.
- `1a19b7c` · Follow-up fix — `ColorizeInternal` start-scan guard added so the closing ``` of a same-token fence closes the span instead of being mis-detected as a new opener.
- `34de078` · Slice 4 — `SelectableTextRun` MVP under `Source_Core/{include,src}/SelectableTextRun.{h,cpp}` with `Begin/EndBlock/RegisterSegment/End` + `GetSelectedText/HasSelection/GetHoveredHref`; `MarkdownPreviewRender::Options::{selectableId,existingSelCtx}` integration; ticket description preview made selectable; bucket-A `SelectableTextRun.test.cpp` adds 9 cases / 13 assertions.
- Slice 5 (this commit) — AI chat moved off `TextEditor` onto `MarkdownPreviewRender` + a single outer `SelectableText::Begin/End` shared across all messages via `existingSelCtx`. `AiChatTextEditorRender.{h,cpp}` deleted. `MarkdownChat()` LD + `SetTokenColor` / `DisableColorizerPasses` vendored-patch methods retired from `TextEditor.{h,cpp}`. Role label rendered as a styled ImGui run + registered as its own selection Segment; the `> You:` / `> Assistant:` raw-source serialiser is gone.

## Deviations from plan

- The trivial helper TU `Plan-doc viewer`'s "lazy file-load on combo-change (single-buffer reuse)" is implemented inside `LoadSelected()` exactly as planned, but the read path uses a local `ReadCapped()` helper rather than reusing `AgentsMdLoader::LoadOneCapped` — the latter applies sentinel formatting tuned for AGENTS.md layering, not plain doc display. A 1 MiB cap with a one-line truncation footer was used instead.
- Slice 3's "On modal save, `std::strncpy(...)`" became an after-render `IsTextChanged()`-gated `std::memcpy` so the existing preview / save / dictation paths keep reading from `s_ActiveLongTextState.Buffer.data()` unchanged. The TextEditor and the raw buffer are kept in sync every frame; equality short-circuits the work when nothing changed.
- The DX12 build was not exercised — `WhisperAiAssistantAutosendScenario.cpp` has a pre-existing `assistantPanelOpen` symbol-mismatch on `develop` unrelated to this work. Standalone build + dual-target Source_Core compile of the changed TUs both pass.
- **Slice 4 MVP scope reduction**. The full plan specified `SelectableText::TextRun` as a from-scratch glyph-emit replacement using `ImDrawList::AddText` + `ImFont::CalcTextSizeA` wrap loops + `ItemAdd`. The ship implementation instead exposes `RegisterSegment` and reuses the caller's existing `ImGui::TextUnformatted` path (MarkdownPreviewRender's per-word loop), recording the just-drawn segment for hit-test + overlay. Smaller blast radius, less risk of off-by-one selection lag (plan R7). `TextRun` is implemented as a thin wrapper over `PushFont/TextUnformatted/RegisterSegment` for callers that don't want to drive the emit themselves. Promote to a true raw-primitive implementation when bucket-C perf shows the per-word ItemAdd cost matters or when self-wrapping becomes necessary. Other deferred items (double-click word-extend, triple-click block-extend, keyboard selection, perCharCumWidth caching, code-block + table selectability, cross-window selection) tracked in the SelectableTextRun.h header doc + the slice-4 commit message.
- **Slice 5 widened in commit scope**. The plan said "Strip `SetTokenColor` + `DisableColorizerPasses` patches as part of the same commit" — done as part of this slice's commit since the AI chat retirement was the last caller. `MarkdownChat()` LD likewise dropped. `Markdown()` LD stays in `TextEditor.cpp` per the plan since slices 2 + 3 still use it.
- **Slice 5 streaming path**. The plan specified the role-line as a "heading-like styled run with role color" replacing the `MarkdownChat()` regex. Implementation does this via an `ImGui::TextUnformatted` with `PushStyleColor` followed by an explicit `SelectableText::RegisterSegment` for the role label, then a `EndBlock` so Ctrl+C inserts a newline between the role and the body. Body bytes go through `MarkdownPreviewRender::Render` with `existingSelCtx` set so all per-word segments register into the same outer Context the history `Begin` opened.
- **`MarkdownPreviewRender::Options` API addition**. The plan called for `selectableId` only; the AI-chat surface needed `existingSelCtx` (a pre-opened Context shared across many sequential Render() calls) so drag-select crosses message boundaries. Both options are now public; `existingSelCtx` wins over `selectableId` when both are set.

## Verification

- Bucket A (doctest) — `MarkdownLanguageDefinition.test.cpp` covers `Markdown()` regex compilation + match shape + same-token fence SetText round-trip (Slice 5 dropped the `MarkdownChat()` cases when the LD was retired). `SelectableTextRun.test.cpp` adds 9 cases / 13 assertions on doc-order range walk + reverse-selection normalisation + block-boundary `\n` insertion + stale-endpoint rejection + `GetHoveredHref`. `ctest --preset ninja-test-msvc` passes (smatchet_tests + smatchet_lua_tests, 100%).
- Bucket B (manual) — Slice 1 + 4 + 5 validated under the AI chat panel ("send me an example of markdown text" prompt against DeepSeek; fence-stuck-open regression fixed; drag-select + Ctrl+C functional after Slice 5). Slice 2 (plan-doc viewer) + Slice 3 (ticket description editor) deferred to follow-up manual smoke.
- Bucket C (perf) — not run; SelectableTextRun's `RegisterSegment` cost is one `string::assign` + a `vector::push_back` per emitted word — expected sub-µs. `perf-measure` against `AiAssistantBigPromptStream` is the canonical bench point; schedule a follow-up.
- Bucket D (sanitizer) — not run in this session; existing ASan/UBSan presets remain wired through `ninja-msvc-asan`.
- Bucket E (ImGui Test Engine) — rig is wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`); drag-select replay + Ctrl+C clipboard assertion remain the canonical bucket-E targets for Slice 4 — no test landed in this slice.
