# Code syntax coloring (multi-language) + tooltips

## Context

Smatchet today renders code (fenced markdown blocks, blame source lines, callstacks, grid-cell tooltips) through two paths:

1. **C/C++ only** via `CppSyntaxHighlight.h` (`DrawColoredCppLine` / `DrawColoredCppText`) — a hand-written tokenizer with keyword / string / comment / number / preprocessor coloring driven by `SmatchetTheme::GetSyntaxColors()`.
2. **Everything else** via `ImGui::TextUnformatted` with a flat orange tint `(0.95, 0.85, 0.6, 1.0)` — Python, Lua, JSON, bash, SQL, HLSL/GLSL all look identical to the user despite the markdown fence tagging them differently.

The vendored `Source_Core/ThirdParty/ImGuiColorTextEdit` library ships `LanguageDefinition` factories for **CPlusPlus / C / Lua / GLSL / HLSL / SQL / AngelScript / Markdown** (audit per `Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.h:148-159`). **Python is NOT vendored** — its keywords / operators / string-form list have to be hand-rolled the same way `Markdown` was added (factory at `TextEditor.cpp:3630`). The editor surface uses these LDs today (`TicketFieldEditor.cpp:1495` wires Markdown into the long-text editor); the read-only viewing path leaves the rest on the floor.

The LDs do NOT expose a public `Tokenize()` method — `TextEditor::Colorize` (the internal tokenizer) is `private:` (`TextEditor.h:256/296`). What an LD does expose: `TokenRegexStrings mTokenRegexStrings` (vector of `std::pair<regex_string, PaletteIndex>`), `mIdentifiers` + `mPreprocIdentifiers` (string→Identifier maps), `mCommentStart` / `mCommentEnd` / `mSingleLineComment` (string literals), `mCaseSensitive` (bool). Slice 1's wrapper builds a small tokenizer on top of these tables — walks the input left-to-right, applies single-line + block-comment shortcuts first, then tries each regex against the remaining input, picks the longest match, looks up identifiers in `mIdentifiers` for the `Identifier` palette slot. This is the same shape `TextEditor::Colorize` uses internally — duplicated here because the internal path is editor-coupled (line buffers, cursor state).

User ask, expanded:
- **Coloring for viewing** — wherever code is *displayed* (not edited), it should be syntax-coloured per its language tag, not just C++.
- **Tooltips** — a hover affordance that tells the user something useful about what they're looking at. Two interpretations bundled (smallest first): (a) hover the code block's language badge to see its full name + lang-detection source ("auto-detected from `.lua` extension"); (b) per-token hover (hover a keyword to see `C++ reserved — compile-time evaluation`). Slice 4 covers (a); slice 5 stretches to (b).

This plan is **sliceable** — each slice ships independently + verifies on its own.

## Files to modify

| File | Change |
|---|---|
| [Source_Core/include/CodeColorView.h](../../Source_Core/include/CodeColorView.h) **(new)** | Public API: `enum class CodeLang { Plain, CPlusPlus, C, Lua, Glsl, Hlsl, Sql, AngelScript, Markdown, Json, Bash, Python };` + `CodeLang CodeLangFromTag(const std::string& tag);` + `void DrawColoredCode(const char* utf8, CodeLang lang);` + `void DrawColoredCodeBlock(const char* utf8, CodeLang lang, const std::string& langTagOrigin = "");` (origin string drives the slice-4 tooltip). Enum order matches the LD-availability tier: vendored-first (CPlusPlus…Markdown), then hand-rolled (Json, Bash, Python) — see § Tokenizer wrapper + hand-rolled LDs below. |
| [Source_Core/src/CodeColorView.cpp](../../Source_Core/src/CodeColorView.cpp) **(new)** | Implementation. Tag→enum table (case-insensitive, alias-rich per slice 1 below). For `CPlusPlus` / `C` routes to existing `DrawColoredCppText` (zero behaviour change for the cpp path). For every other vendored LD (`Lua`, `Glsl`, `Hlsl`, `Sql`, `AngelScript`, `Markdown`), runs the LD-walking tokenizer (§ Tokenizer wrapper) over the input + emits `ImGui::Text` per token coloured from a `PaletteIndex`→`SmatchetThemeSyntaxColors` field map. For `Json` / `Bash` / `Python` — hand-rolled minimal LDs constructed in `CodeColorView.cpp` (same shape as `TextEditor::LanguageDefinition::Markdown()` at `TextEditor.cpp:3630`). `Plain` falls through to `ImGui::TextUnformatted` with the existing flat-orange tint. |
| [Source_Core/src/MarkdownPreviewRender.cpp:847-889](../../Source_Core/src/MarkdownPreviewRender.cpp) | Replace the `isCpp ? DrawColoredCppText : flat-orange-TextUnformatted` branch with `CodeColorView::DrawColoredCodeBlock(b.codeBuffer.c_str(), CodeColorView::CodeLangFromTag(b.codeLang), b.codeLang)`. Code-block layout (Copy button, child window, BG color) unchanged. |
| [Source_Core/include/MarkdownPreviewRender.h:146-155](../../Source_Core/include/MarkdownPreviewRender.h) | `IsCppLikeLangTag` becomes a thin wrapper over `CodeColorView::CodeLangFromTag(lang) == CodeLang::CPlusPlus || == CodeLang::C` so existing test coverage continues to apply. |
| [Source_Core/src/BlameAnalysisUi_Window.cpp:317,689](../../Source_Core/src/BlameAnalysisUi_Window.cpp) | `DrawColoredCppText` / `DrawColoredCppLine` calls stay (they're explicitly C++-context); but **new** lang-from-file-extension helper used when the blame target isn't `.cpp`/`.h` (e.g. `.lua`, `.py`). Slice 5. |
| [Source_Core/src/SmatchetFieldRender.cpp:42-56](../../Source_Core/src/SmatchetFieldRender.cpp) | Same — tooltip path swaps from `DrawColoredCppText` to `CodeColorView::DrawColoredCode(tipSource, CodeColorView::CodeLangFromTag(detectedLang))` once the helper supports field-content lang detection. Slice 5. |
| [Source_Core/include/SmatchetTheme.h:7-13](../../Source_Core/include/SmatchetTheme.h) | `SmatchetThemeSyntaxColors` stays as-is — the 5-token palette (Keyword/String/Comment/Number/Preprocessor) maps cleanly onto every `LanguageDefinition`'s `PaletteIndex` (no new tokens needed for slice 1-3). |
| [tests/Source_Core/CodeColorViewLang.test.cpp](../../tests/Source_Core/CodeColorViewLang.test.cpp) **(new)** | doctest pinning `CodeLangFromTag` for every alias listed in slice 1 step 2 + one round-trip per hand-rolled LD (Python / Bash / Json) asserting the palette-index histogram. Pure-logic for the alias map; the LD-tokenizer tests pull `TextEditor.cpp`'s LD factories at link time but no ImGui context is needed (the tokenizer outputs Token vectors, ImGui calls live in the render layer). |
| [tests/CMakeLists.txt](../../tests/CMakeLists.txt) | Register new test TU. |

## Existing utilities to reuse

- `SmatchetTheme::GetSyntaxColors()` — per-theme palette, already populated for all 7 themes.
- `DrawColoredCppText` / `DrawColoredCppLine` — unchanged; `CodeColorView` routes through it for the C++ branch so no behaviour change on that path.
- `TextEditor::LanguageDefinition::{CPlusPlus, C, Lua, Glsl, Hlsl, Sql, AngelScript, Markdown}()` — vendored static factories at `TextEditor.h:148-159`. **`Python()` is NOT vendored** — hand-rolled below.
- `MarkdownPreviewRender::IsCppLikeLangTag` — kept as a thin delegate so its 8-alias coverage stays asserted by the existing test rig.

## Tokenizer wrapper

`TextEditor::LanguageDefinition` exposes only descriptive tables, not a public `Tokenize()` method. The shape we read off the LD:

- `TokenRegexStrings mTokenRegexStrings` — `vector<pair<regex_string, PaletteIndex>>`.
- `mIdentifiers`, `mPreprocIdentifiers` — `unordered_map<string, Identifier>` for keyword + preproc lookup.
- `mCommentStart`, `mCommentEnd`, `mSingleLineComment` — block + line comment delimiters.
- `mCaseSensitive` — affects identifier lookup.

Slice 1 ships `CodeColorView::TokenizeWithLd(const std::string& src, const TextEditor::LanguageDefinition& ld, std::vector<Token>& outTokens)`. Single-pass left-to-right scan: (a) emit any leading whitespace as a `Whitespace` palette token (no colour, preserves layout); (b) if `mSingleLineComment` matches → consume to end-of-line as `Comment`; (c) if `mCommentStart` matches → consume to `mCommentEnd` as `MultiLineComment`; (d) try each regex in `mTokenRegexStrings`, pick the longest match, emit with its `PaletteIndex`; (e) for `Identifier` palette index, also look up in `mIdentifiers` and re-tag to `KnownIdentifier` if found (matches `TextEditor::Colorize` behaviour). When no regex matches the leading char, advance one char as `Default` (defensive — should not happen on well-formed input). Output is cached per `(content_hash, lang_enum, theme_revision)`; cache shape mirrors the existing `s_messageHeightCache` in `SmatchetAiAssistantUi.cpp`.

The cache key + cap (256 KiB max input, 256-entry FIFO eviction) keep this within Pillar 2 budget. The tokenizer itself is bounded by `mTokenRegexStrings.size()` (~10-20 per LD) and input length (capped). No I/O.

## Hand-rolled LDs (Python / Bash / Json)

These don't ship in `TextEditor.cpp`; slice 1 adds them to `CodeColorView.cpp` as file-local `static const TextEditor::LanguageDefinition&` factory functions, same pattern as the vendored `Markdown()` factory at `TextEditor.cpp:3630`. Scope per language:

- **`Python`** — 35 reserved words (`def`, `class`, `if`, `elif`, `else`, `for`, `while`, `return`, `import`, `from`, `as`, `with`, `try`, `except`, `finally`, `raise`, `yield`, `lambda`, `pass`, `break`, `continue`, `global`, `nonlocal`, `async`, `await`, `True`, `False`, `None`, `and`, `or`, `not`, `is`, `in`, `del`, `assert`); 30 builtins (`print`, `len`, `range`, `list`, `dict`, `set`, `tuple`, `int`, `str`, `float`, `bool`, `type`, `isinstance`, `enumerate`, `zip`, `map`, `filter`, `sorted`, `reversed`, `min`, `max`, `sum`, `any`, `all`, `open`, `super`, `iter`, `next`, `repr`, `hash`); regex for `#`-line-comments, `'''...'''` / `"""..."""` triple-string blocks, `'...'` / `"..."` single-line strings, integer + float literals, identifiers. Decorators (`@foo`) tagged as `Preprocessor`.
- **`Bash`** — 25 reserved words (`if`, `then`, `else`, `elif`, `fi`, `case`, `esac`, `for`, `while`, `until`, `do`, `done`, `function`, `select`, `return`, `break`, `continue`, `in`, `time`); 20 common builtins (`echo`, `cd`, `pwd`, `export`, `source`, `alias`, `unalias`, `local`, `readonly`, `declare`, `unset`, `read`, `set`, `shift`, `trap`, `exec`, `eval`, `exit`, `wait`, `test`); regex for `#`-line-comments, `'...'` / `"..."` strings, `$VAR` / `${VAR}` variable refs (tag `Preprocessor`), integer literals.
- **`Json`** — only 3 keywords (`true`, `false`, `null`); regex for `"..."` strings, integer + float literals, structural punctuation (`{` `}` `[` `]` `:` `,`) as `Punctuation` palette. No comments (strict JSON forbids them; JSONC is a separate ask, out of scope).

These three add ~150 LoC total to `CodeColorView.cpp` (50 each, mostly keyword arrays). Slice 1 doctest covers the alias mapping + at least one round-trip per hand-rolled LD ("input → token stream → palette-index histogram").

## Pillar callouts

### Pillar 1 — Performance

`DrawColoredCode` is on the AI-chat render path (per-frame for visible code blocks) + the blame view (one call per visible source line, currently capped at ~50 visible lines per scroll viewport). Budget: must stay sub-µs per line at the 6.94 ms frame budget.

The vendored `LanguageDefinition::Tokenize` is regex-based — naive per-frame tokenization would re-compile + re-run regexes 60× per second per visible block. **Cache invariant**: every code-block plan node gets a tokenized-cache slot keyed by `(content_hash, lang_enum)`. First render parses; subsequent frames replay. Same shape as the existing `s_messageHeightCache` per-message cache in `SmatchetAiAssistantUi.cpp`. Cache invalidates when `assistantHistory` size changes (already wired) and when theme changes (new — hook into `SmatchetTheme::ApplyStyle` post-apply callback).

Measurement plan in § Perf-review-system gates below.

### Pillar 2 — UI never freezes

All work is synchronous CPU on the UI thread. No I/O, no SQLite, no HTTP. Bound: tokenization is O(N) in code-block length × regex complexity. The cache makes steady-state cost O(1). Worst-case first-frame cost on a 4 KiB code block: measured-budget guard ≤ 5 ms (sub-frame; well under the 100 ms blocking-call ceiling).

### Pillar 3 — Never crash

`LanguageDefinition` from upstream is well-tested. Risks: malformed UTF-8 in the input could trip the regex engine; cap input at 256 KiB per block + truncate with a visible `[...truncated]` tail. Tokenizer wrapped in `try/catch (const std::exception&)` per AGENTS.md graceful-degradation rule; failure falls back to `Plain` rendering + `LOG_WARN`.

### Pillar 4 — Accessibility

- Colour-only differentiation: 5-token palette already meets WCAG AA contrast against window bg on every shipped theme (audited in PR #341's perf-baseline sweep). The new path inherits that audit because it uses the same `SmatchetThemeSyntaxColors`.
- Keyboard nav: read-only views are non-focusable; the slice-4 lang-badge tooltip activates on `ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)` so keyboard-focus alternative is via the existing tab order (no regression).
- Tooltips display LD canonical name + lang-detection origin so screen-reader text-only mode (future) has prose to read.

## Perf-review-system gates

Per AGENTS.md § "Plan-doc perf-gate section — mandatory when diff touches `Source_Core/`".

**Fires:**
- `ai-chat-history-render` scenario (already baselined at `docs/perf/baselines/ai-chat-history-render.dev.json`) — the AI chat panel exercises markdown code blocks every frame when the panel is open + history non-empty. Slice 2 must not regress this scenario's `lastTotalMs` past the regression-policy threshold.
- `idle` scenario — universally reachable; sanity gate.
- New `code-color-render` scenario candidate (slice 1 deliverable, optional): seeds N=20 mock code blocks across 5 languages, drives M=300 frames; emits `rows[]` per the post-PR-#343 contract (`UiPerfMonitor::Instance().GetLastFrameRows()`). Baseline captured via `bash scripts/dev/perf-baseline.sh init code-color-render` after slice 1 lands.

**Doesn't apply (one-line reason each):**
- `priority-grid-scroll`, `cell-edit-burst` — grid scenarios; no code-block rendering involved.
- `theme-switch-roundtrip`, `dock-gap-sentinel`, `command-palette-fuzzy` — bucket-C-only, not perf-emitting (tracked at `tooling.md` "8 of 15").
- `whisper-*`, `agent-*` — dictation / agentic surfaces; orthogonal.

**Recommended pre-push local check** (per AGENTS.md § Perf slice-boundary auto-run):

```bash
bash scripts/dev/perf-run.sh ai-chat-history-render
python scripts/dev/perf-compare.py \
    docs/perf/baselines/ai-chat-history-render.dev.json \
    build/perf-runs/ai-chat-history-render-<ts>.json --markdown-only
```

PR-time CI gate handles the same check via `.github/workflows/perf-pr-fast.yml` (the scenario is in `scripts/dev/perf-pr-fast-set.json`).

## Implementation slices

Each slice ships as its own PR, independently mergeable. Author one at a time; orchestrator picks up at the next slice after the prior PR merges.

### Slice 1 — `CodeColorView` substrate (no consumer changes)

1. `Source_Core/include/CodeColorView.h` + `.cpp` per the file table.
2. `CodeLangFromTag` aliases:
   - `cpp` / `c++` / `cxx` / `cc` / `hpp` / `h` / `hh` / `hxx` → `CPlusPlus`
   - `c` → `C` (note: `h` headers go to CPlusPlus because Smatchet's `.h` files are C++ — back-compat with the existing `IsCppLikeLangTag` coverage that treats `.h` as cpp-like)
   - `lua` → `Lua`
   - `glsl` / `vert` / `frag` → `Glsl`
   - `hlsl` → `Hlsl`
   - `sql` → `Sql`
   - `as` / `angelscript` → `AngelScript`
   - `md` / `markdown` → `Markdown`
   - `json` / `jsonc` → `Json` (hand-rolled; jsonc tag accepted as alias even though strict JSON parser is shipped — caller wants comments-tolerant view)
   - `bash` / `sh` / `zsh` / `shell` → `Bash` (hand-rolled)
   - `py` / `python` / `python3` → `Python` (hand-rolled)
   - "" / `txt` / `text` / `plaintext` / unknown → `Plain`
3. `DrawColoredCode` for `CPlusPlus` / `C` delegates to the existing `DrawColoredCppText` verbatim (zero behaviour change). For `Plain`, calls `ImGui::TextUnformatted` with the existing flat-orange tint. Every other lang routes through the § Tokenizer wrapper above + caches the token vector keyed by `(content_hash, lang_enum, theme_revision)` in a process-static `std::unordered_map` (256-entry FIFO eviction, 256 KiB input cap).
4. Hand-rolled LDs for Python / Bash / Json per § Hand-rolled LDs section above.
5. `tests/Source_Core/CodeColorViewLang.test.cpp` pins every alias mapping + at least one round-trip per hand-rolled LD asserting the palette-index histogram (e.g. Python `def foo(): pass` → ≥2 Keyword, ≥1 Identifier).
6. Build clean (Standalone + DX12) + ctest green. No UI surface change yet.

### Slice 2 — wire `MarkdownPreviewRender` code-block path through `CodeColorView`

1. Replace the `isCpp ? DrawColoredCppText : flat-orange-TextUnformatted` branch in `MarkdownPreviewRender.cpp:847-889` with `CodeColorView::DrawColoredCodeBlock(b.codeBuffer.c_str(), CodeColorView::CodeLangFromTag(b.codeLang), b.codeLang)`.
2. `IsCppLikeLangTag` becomes the thin wrapper described above so existing tests stay green.
3. Auto perf-run per the slice-boundary rule (`ai-chat-history-render` baseline gate).
4. Visual-validation pause per the AGENTS.md visual-validation exception (touches `SmatchetAiAssistantUi.cpp` render path via MarkdownPreviewRender; no existing bucket-C/E for multi-lang code-block coloring). Pause after build → launch exe → user evaluates Python / Lua / JSON / bash code blocks in chat (need messages containing those — orchestrator seeds via `dispatchSend` with markdown text or asks user to send) → resume on "looks good".

### Slice 3 — theme-swap cache invalidation

1. Add a process-static `std::atomic<std::uint64_t> g_theme_revision` bumped inside `SmatchetTheme::ApplyStyle` after the per-theme write completes.
2. `CodeColorView`'s tokenization cache snapshots the revision per entry; cache miss when the entry's revision != current.
3. Doctest: switch theme → re-render → assert cache rebuilt (verifiable via a debug-only `GetCacheRebuildCount()` accessor gated on `SMATCHET_BUILD_TESTS`).

### Slice 4 — language-badge tooltip on code blocks

1. In the code-block render (`MarkdownPreviewRender.cpp` post-slice-2), inject a small ghosted `ImGui::TextDisabled("{lang}")` label at the top-right corner of the code-block child window (or in the Copy button's row). When `lang.empty()`, label is `plain`.
2. `ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)` shows tooltip body: `"{LD canonical name}\n(detected from markdown fence tag: `{langTagOrigin}`)"`. When `langTagOrigin.empty()`: `"(no language tag — rendering as plain text)"`.
3. No new buckets — visual change is small, gates on slice 2's bucket-C/E coverage (if added) plus visual-validation re-pause.

### Slice 5 (stretch) — per-token tooltips + extend beyond markdown

5a. `BlameAnalysisUi_Window.cpp` — small helper `CodeLang LangFromFilePath(const std::string& path)` (uses extension only: `.cpp`/`.h` → CPlusPlus, `.py` → Python, `.lua` → Lua, etc.) + swap the C++-hardcoded `DrawColoredCppText` for the polymorphic `DrawColoredCode`.

5b. `SmatchetFieldRender.cpp` — same swap on the grid-cell-tooltip path when the field's `valueType` is a text-with-language hint (today: never; future: when tracker schemas grow such metadata).

5c. Per-token tooltips — extend `LanguageDefinition::Tokenize`'s output to also retain per-token (rect, type) tuples; render an `ImGui::IsItemHovered` per token with a 1-line description (`"C++ reserved word — compile-time evaluation"` etc.). Adds layout cost (one `IsItemHovered` per token; bounded by per-line token count which caps at ~30 on typical code). Defer if slice 3's perf baseline shows headroom < 2 ms per frame on the worst-case code-block-heavy scenario.

## Risks + mitigations

| Risk | Mitigation |
|---|---|
| Regex-based tokenization burns CPU per frame. | `(content_hash, lang, theme_revision)`-keyed cache; first-frame parse, replay thereafter. Cap input at 256 KiB. |
| Theme switch leaves stale colours in the cache. | Slice 3 explicit theme-revision invalidation. |
| Per-language palette mismatch — e.g. SQL has no "preprocessor" concept. | `SmatchetThemeSyntaxColors` 5-token palette covers every LD's distinct categories; LD types that don't map (e.g. SQL's `Keyword2`) fall back to `Keyword`. Document the mapping in `CodeColorView.cpp` header comment. |
| Hand-rolled LDs (Python / Bash / Json) drift behind upstream syntax additions. | Lists are pinned in `CodeColorView.cpp` (`kPythonKeywords[]`, `kBashKeywords[]`, etc.) + asserted by the slice-1 doctest's palette-histogram check. Drift is visible (the doctest fails when a new keyword is added without the array). When upstream `ImGuiColorTextEdit` adds a vendored `Python()` factory, swap to it + delete the hand-rolled tables — one-PR migration. |
| `TextEditor::LanguageDefinition::Markdown()` already wired into the editor surface (`TicketFieldEditor.cpp:1495`). | This is read-only; uses the same factory in viewing mode. Two independent consumers OK — `LanguageDefinition` is immutable post-construction (static + `const &` return). No race. |
| Custom `LanguageDefinition`s shipped by future plugins clash. | API exposes only the enum; plugin-defined LDs go through a future `RegisterCodeLang(name, LD)` extension point (out of scope for this plan). |
| Pillar 4 colour-contrast regression on HighContrast theme. | HighContrast's syntax palette already audited at AAA in PR #341's sweep. New code paths use the same palette → no new audit needed. |
| Tooltips race with code-block scrolling. | ImGui's tooltip uses `IsItemHovered`; while the user is scrolling, the hovered-item state correctly suppresses tooltip render. Pre-existing pattern; no new risk. |

## Verification

Per AGENTS.md § Verification automation — zero manual steps. Each slice's verification:

**Slice 1 (bucket A only — pure-logic):**
- `tests/Source_Core/CodeColorViewLang.test.cpp` pins every alias (~25 cases). doctest, no ImGui.
- Build clean Standalone + DX12.
- Manual residue: none.

**Slice 2 (bucket A + B + automated perf):**
- A: extend the slice-1 test with `CodeLangFromTag` invariants (case-insensitive, alias-stability).
- B (perf): `bash scripts/dev/perf-run.sh ai-chat-history-render` post-build; compare against the dev-host baseline; FAIL if `drawAiAssistantPanel` outer lifts by > policy threshold.
- E (deferred — visual-validation pause): user signs off on multi-lang code blocks rendering in chat with distinct colours.

**Slice 3 (bucket A):**
- doctest using the `GetCacheRebuildCount` debug accessor: render → switch theme → render → assert count incremented.

**Slice 4 (bucket E candidate):**
- `tests/ui/code_block_lang_tooltip.test.cpp` (bucket-E ImGui Test Engine): seed an AI chat message with `\`\`\`python\nprint(1)\n\`\`\``, hover the lang badge, assert tooltip body matches `"Python\n(detected from markdown fence tag: \`python\`)"`. If bucket-E setup is non-trivial: deferred to a `tooling`-category backlog entry, with a one-screen manual-validation contract in the slice's PR body as the interim.

**Slice 5 (stretch):**
- Per-token tooltip coverage gated on slice 4's bucket-E shape working; same engine, more assertions.

## Out of scope (explicit deferrals)

- **Code-block run-time execution** — render-only. No `eval`, no language-runtime spawn.
- **In-place editing of viewed code** — `ImGuiColorTextEdit` is for editing; this plan is for viewing. Editing surfaces (ticket field long-text editor) keep their existing `TextEditor` shape.
- **Syntax-aware diff highlighting** — a different feature (red-add / green-remove line markers). Track separately if requested.
- **Theme-token additions** — sticking to the existing 5-token palette. If a language needs a 6th token (e.g. "annotation" for Python decorators), file a `process`-category backlog entry rather than mid-flight palette growth.
- **Plugin-registered LanguageDefinitions** — out of scope; future `RegisterCodeLang` API.
- **Screen-reader textual code description** — Pillar 4 § out of scope, until ImGui ships an a11y tree.

## Implementation log

All slices shipped. Paths moved from the planned `Source_Core/` to `Source/Core/`
after the source-root consolidation (#552); the symbols and shapes match the plan.

- **Slice 1 — `CodeColorView` substrate** (shipped). `Source/Core/include/Ui/CodeColorView.h`
  + `Source/Core/src/Ui/CodeColorView.cpp` (ns `smatchet::code_color`): `CodeLang`
  enum, `FromTag` alias map, `LangFromFilePath`, `CanonicalName`, the LD-walking
  `Tokenize`, hand-rolled Python/Bash/JSON LDs, and the `(content_hash, lang,
  theme_revision)`-keyed cache. Doctest `tests/Core/CodeColorViewLang.test.cpp`.
- **Slice 2 — markdown code-block wiring** (shipped). `MarkdownPreviewRender.cpp`
  routes every fenced block through `DrawColoredCodeBlock`; non-empty tags that
  `FromTag` classifies `Plain` fall back to `LangFromFilePath` (CR #353 — handles
  `foo.py`-style fence names).
- **Slice 3 — theme-swap cache invalidation** (shipped). `SmatchetTheme.cpp` carries
  the monotonic `g_themeRevision` atomic, bumped in `ApplyStyle`; exposed via
  `SmatchetTheme::GetThemeRevision()`; the cache key snapshots it so a theme switch
  misses and re-resolves the palette. Doctest cases in `CodeColorViewLang.test.cpp`
  (`GetCacheRebuildCountForTest` / `TokenizeCachedForTest`).
- **Slice 4 — language-badge tooltip** (shipped). `MarkdownPreviewRender.cpp` draws a
  ghosted `TextDisabled` badge + `DelayNormal` hover tooltip naming the LD + the
  fence-tag origin.
- **Slice 5a — annotate-view wiring** (shipped). `AnnotateAnalysisUi_Window.cpp`
  consumes `CodeColorView`.
- **Golden gate** (this PR, #651, merge `99318756`). `CodeSyntaxColoringScenario`
  renders C++ / Python / Lua / Bash / JSON / Plain via `DrawColoredCodeBlock` into a
  deterministic opaque window + triggers the PNG capture; registered in
  `SmatchetScenarioRegistry.cpp` (+ stub + snapshot test), wired into
  `scripts/dev/test-screenshot-diff.sh`, golden at
  `tests/golden/code-syntax-coloring.png`. Closes the deferred golden-image gap.

## Deviations from plan

- **Paths**: plan wrote `Source_Core/…`; the tree is `Source/Core/…` post-#552. No
  semantic change.
- **`SmatchetThemeSyntaxColors` grew an `Identifier` field** (slice 6, unplanned at
  authoring): identifiers previously fell through to `ImGuiCol_Text` and read as
  uncoloured on identifier-dominated views; a per-theme `Identifier` tint was added.
- **Slice 5b/5c not shipped**: `SmatchetFieldRender` grid-cell-tooltip wiring (5b) and
  per-token hover tooltips (5c) remain deferred — no consuming tracker schema for 5b
  yet, and 5c is gated on a perf-headroom check. Tracked in the backlog.
- **Known live bug (separate track)**: code-color slices 5+6+7 (#353) shipped with the
  long-text editor + callstack-tooltip paths not visibly colouring (missing
  `editor.Colorize(0, -1)` after `SetLanguageDefinition`; the annotate path calls it
  and works). Filed P1 in `docs/self-improvement/categories/bug.md` (2026-05-21);
  fix queued as a follow-up. NOT addressed by this golden PR, which covers the
  markdown `DrawColoredCodeBlock` path only.

## Verification (actual)

- Standalone built clean (MSVC iter) in an isolated worktree; the new scenario TU
  compiled into `SmatchetStandalone`.
- `cmd scenario.run --name=code-syntax-coloring` returned
  `captureRequested:true, sampleCount:6, ok:true`.
- The captured render was reviewed and approved by the repo owner before the golden
  was committed (golden-image-approval contract): per-language keyword / string /
  comment / number / identifier colouring distinct; Plain renders flat-orange.
- PR #651 CI green — Windows+MSVC (full + light), Bucket-C/E (Mesa headless GL),
  ASAN, Perf-PR-fast, CodeRabbit 0-actionable. Squash-merged as `99318756`.
