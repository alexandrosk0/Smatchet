# Extend ImGuiColorTextEdit — `LanguageDefinition::Markdown()` + 3 consumers

## Context

Today, markdown in `TextEditor` (vendored `Source_Core/ThirdParty/ImGuiColorTextEdit/`) is highlighted only inside the AI chat panel via the **hand-rolled** `AiChatMarkdownTokens` scanner (PR #289, plan [`ai-chat-textedit-markdown.md`](ai-chat-textedit-markdown.md)). That scanner runs a state machine + `std::regex` pass and writes per-glyph palette indices through the patched `TextEditor::SetTokenColor`. The built-in `ColorizeInternal()` is force-disabled via `DisableColorizerPasses()` to keep it from clobbering manual spans.

That works for AI chat but isn't reusable: any other markdown surface (plan-doc viewer, ticket description editor, markdown preview edit-mode) has to re-implement coloring, or wire `AiChatMarkdownTokens` even though its enums (`UserRoleMarker` / `AssistantRoleMarker` / `UserMsgBody`) are chat-specific.

[Issue #155 on BalazsJako/ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit/issues/155) gives a different mechanism — a `LanguageDefinition::Markdown()` that plugs into TextEditor's **native** regex pipeline. Every existing built-in (`Lua()`, `CPlusPlus()`, etc.) ships as a `LanguageDefinition`; `SetLanguageDefinition()` is the documented seam. A markdown LD makes coloring available to **every** TextEditor instance with one line.

**User decisions** (2026-05-19):
- Outcome: **add LD + migrate AiChat** to it (single colorization mechanism).
- Surfaces: ship the LD, then wire plan-doc viewer + ticket description editor + markdown preview edit-mode.

Outcome: one canonical markdown highlighter, four surfaces using it (AiChat migrated, plan-doc viewer new, ticket description editor migrated, markdown preview edit-mode new).

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
| `tests/Source_Core/AiChatMarkdownTokens.test.cpp` | **Delete** the chat-tokens cases; replace with bucket-A doctest on the LD's regex strings (compile each regex once, assert match shape on representative inputs). The colorizer's per-glyph palette is integration territory (bucket E, deferred). |
| `tests/CMakeLists.txt` | Drop the `AiChatMarkdownTokens.cpp` source + `.test.cpp` entries; add `MarkdownLanguageDefinition.test.cpp`. |
| `CMakeLists.txt` (root) | Remove `AiChatMarkdownTokens.cpp` from `CORE_SOURCES`. |
| `Source_Core/src/TicketFieldEditor.cpp` (long-text editor modal) | Replace the description-edit-mode `ImGui::InputTextMultiline` (line 1340, `"##LongTextEditorBuf"`) with a `TextEditor` instance configured with `Markdown()` LD. Keep the existing preview-mode `MarkdownPreviewRender` path (md4c-based rich render — different surface from raw-source coloring). Toggle between the two via the existing modal toggle. |
| `Source_Core/include/SmatchetPlanDocViewerUi.h` (new) | Public `DrawPlanDocViewer(UiDrawSession&)` declaration + toggle flag added to `UiDrawSession` (`showPlanDocViewer`). |
| `Source_Core/src/SmatchetPlanDocViewerUi.cpp` (new) | New read-only `TextEditor` viewer for `docs/design/*.md` and `docs/adr/*.md`. File picker combo (sorted alpha) + `TextEditor` set to `ReadOnly` + `Markdown()` LD + `SetShowLineNumbers(false)`. Reads from disk via `ghc::filesystem::directory_iterator` (or `std::ifstream` — match the codebase's prevailing pattern). |
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

## Slice plan — three sequential PRs

The four surfaces split into three slices (the LD lands once; AiChat migration is gated on it; the three new consumers can ship in parallel after that). Per the `Build / ctest cadence` rule each slice runs build + `scripts/dev/test-all.sh` once at the slice boundary.

### Slice 1 — `feat(textedit): LanguageDefinition::Markdown() + MarkdownChat()`

1. Patch `TextEditor.h` + `TextEditor.cpp`:
   - Add `Markdown()` + `MarkdownChat()` static factories (additive).
   - Apply same-token block-delimiter guard in `ColorizeInternal()` at line ~2054 (additive ~6 LOC; no behaviour change for existing LDs).
2. New bucket-A test `tests/Source_Core/MarkdownLanguageDefinition.test.cpp` — compile each `mTokenRegexStrings` entry via `std::regex(...)`; assert match/non-match on representative inputs (heading w/ space pass, `#foo` no-space fail, `**bold**` matches Bold not Italic, ordered-list marker, role line for `MarkdownChat()`).
3. Migrate AiChat: drop `AiChatMarkdownTokens.{h,cpp}` + test, swap `AiChatTextEditorRender.cpp` to `SetLanguageDefinition(MarkdownChat())` + `SetColorizerEnable(true)`. Visual smoke against existing AI chat panel (Smatchet.exe → DeepSeek prompt).
4. **Patch survivors**: `SetTokenColor` + `DisableColorizerPasses` are retired by this slice (no more callers). Keep both for now (additive, harmless, useful escape hatch); document a follow-up cleanup ticket. `SetShowLineNumbers` stays (used by the new plan-doc viewer + ticket modal).

### Slice 2 — `feat(ui): plan-doc viewer (docs/design/*.md, docs/adr/*.md)`

5. New `SmatchetPlanDocViewerUi.{h,cpp}` — file picker (combo over `docs/design/*.md` + `docs/adr/*.md`, glob via `ghc::filesystem::directory_iterator`) + read-only `TextEditor` with `Markdown()` LD + `SetShowLineNumbers(false)` + `SetReadOnly(true)`. Lazy file-load on combo-change (single-buffer reuse).
6. Register `view.toggle.plan_doc_viewer` in `ViewToggleCommands.cpp` matching the existing snake_case pattern. Add `showPlanDocViewer` toggle to `UiDrawSession`. Bind a menu item under View → Plan docs.

### Slice 3 — `feat(ui): markdown-coloured edit mode for description fields`

7. `TicketFieldEditor.cpp` long-text editor modal at line 1340 (`ImGui::InputTextMultiline("##LongTextEditorBuf", s_ActiveLongTextState.Buffer.data(), ...)`): replace edit-mode `InputTextMultiline` with a function-local `static TextEditor` configured with `Markdown()` LD (writable, `SetShowLineNumbers(false)`). On modal open, `editor.SetText(std::string(s_ActiveLongTextState.Buffer.data()))`. On modal save, `auto text = editor.GetText(); std::strncpy(s_ActiveLongTextState.Buffer.data(), text.c_str(), kBufferSize - 1);` (preserves the existing 64 KiB buffer cap).
8. Preview-mode (md4c `MarkdownPreviewRender::Render`) untouched. The modal's edit/preview toggle now swaps `TextEditor` ↔ `MarkdownPreviewRender`. The `##WorklogDesc` `InputTextMultiline` at line 1168 stays as plain-text input — worklogs are short comments, not markdown.

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
| A — doctest | `tests/Source_Core/MarkdownLanguageDefinition.test.cpp` — compile each `mTokenRegexStrings` entry into `std::regex`; assert match against representative valid input + non-match against near-miss input (e.g. `#foo` (no space) must NOT match heading; `**foo*` must NOT match bold). |
| B — manual | (1) Smatchet → AI assistant panel: ask DeepSeek a multi-block prompt; verify heading / bold / code coloured + drag-select across messages still works (regression check on Slice 1). (2) View → Plan docs: select `ai-chat-textedit-markdown.md`; verify headings + lists + fenced code + links coloured (Slice 2). (3) Open a Jira ticket with a markdown description; toggle edit mode; verify coloring + edit + save round-trip (Slice 3). |
| C — perf | `perf-measure` against `scenario.run "AiAssistantBigPromptStream"` — confirm per-frame steady-state stays ≤ 6.94 ms (Pillar 1). Native LD colorizer is per-line incremental — expected to be cheaper than the hand-rolled per-rebuild full re-tokenisation. |
| D — sanitizer | `cmake --build --preset ninja-test-msys2` ctest under ASan/UBSan after each slice. |
| E — ImGui Test Engine | Per the existing `docs/backlog/agent-self-improvement/tooling.md` deferral. Slice 1 + 2 visual verification documented in PR body. |

## Risks

- **R1 — regex performance on large buffers**: `std::regex` is slow on libstdc++/MinGW UCRT; `ColorizeInternal()` runs all regexes per-line on each colorize pass. Mitigation: TextEditor already re-colorizes only changed line ranges (`mColorRangeMin/Max`); for the plan-doc viewer the document loads once. For the AiChat panel the stream-tail recolour is the only hot path — confirm via `perf-measure` (bucket C above). Fallback: keep `DisableColorizerPasses()` + the hand-rolled scanner alive in a `git revert` if perf regresses.
- **R2 — losing HTML comment coloring**: documented trade-off (decision section). Acceptable because fenced code wins on usage frequency in our corpus.
- **R3 — italic regex over-eager**: `\*[^\*\n]+\*` matches inside list lines starting with `*` if not anchored carefully. The list-marker regex captures the leading `* ` first (different precedence slot), but the italic regex on the line tail can still match a stray `*`. Mitigated by the `[^\*]` body class — unmatched markers won't span. Verify via doctest case.
- **R4 — `MarkdownChat()` regex order**: the role-line regex must precede the generic blockquote regex in `mTokenRegexStrings` (insertion order = match precedence). Slice 1 doctest asserts the wrapper LD's first regex is the role one.
- **R5 — TextEditor as editable widget**: `TextEditor` started life as a code editor; tab key inserts a tab, Enter inserts a newline, no auto-grow. Acceptable for ticket description editing (matches LuaConsole behaviour). Mitigation: document Ctrl+A / Ctrl+C / native edit shortcuts in the modal tooltip.
- **R6 — Plan-doc viewer scope creep**: a "select a doc and view it" widget is one combo + one TextEditor — keep it that. No favourites, no per-file recent list, no in-doc TOC in slice 2.

## Out of scope

- Replacing `MarkdownPreviewRender` itself (md4c rich rendering with clickable links + heading scaling stays for preview surfaces — that's an orthogonal capability).
- Editor-mode bold/italic toolbar buttons, image-paste, drag-drop of attachments.
- Live preview side-by-side (could be a future iteration on slice 3).
- `IsCppLikeLangTag()` fenced-code C++ syntax dispatch — only applies to the md4c preview render, not the LD edit-mode.

## Open questions (low-priority)

- **Q1 — Plan-doc viewer file scope**: `docs/design/*.md` + `docs/adr/*.md` only, or all of `docs/**/*.md`? Default: design + ADR only (those are the curated long-form docs).
- **Q2 — Ticket description modal**: TextEditor as default edit widget, or opt-in toggle? Default: replace outright. Old `InputTextMultiline` path deleted.
- **Q3 — Strip `SetTokenColor` + `DisableColorizerPasses` patches after slice 1 lands**? Default: keep them (additive, harmless, useful escape hatch). Mark deprecation in a comment.
