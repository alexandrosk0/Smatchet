# Plan — Theme-driven C++ syntax highlighting across all code-rendering sites
<!-- index-summary: Per-theme C++ syntax palette extracted from `BlameUiThemeColors`; generic `CppSyntaxHighlight` TU shared by blame + markdown + field renderers. Fully shipped (see plan § Shipped PRs). -->

## Context

Smatchet has a working C++ tokenizer + colorizer in [BlameSyntaxHighlight.cpp](Source_Core/src/BlameSyntaxHighlight.cpp) — `BlameDrawColoredCppLine(const char* utf8Line, const BlameUiThemeColors& theme)` at [BlameSyntaxHighlight.h:7](Source_Core/include/BlameSyntaxHighlight.h:7). It is wired into exactly **one** call site — the Blame UI Entry tab annotated source table at [BlameAnalysisUi_Window.cpp:694](Source_Core/src/BlameAnalysisUi_Window.cpp:694).

Every other place that displays code renders flat text:

- Callstack grid cell + tooltip via [SmatchetFieldRender.cpp:7-47](Source_Core/src/SmatchetFieldRender.cpp:7) (`RenderClippedFieldText`).
- Blame UI raw-callstack read-only display at [BlameAnalysisUi_Window.cpp:316-323](Source_Core/src/BlameAnalysisUi_Window.cpp:316) (`InputTextMultiline` with `ImGuiInputTextFlags_ReadOnly`).
- Markdown fenced ` ```cpp ` code blocks at [MarkdownPreviewRender.cpp:565-597](Source_Core/src/MarkdownPreviewRender.cpp:565) (`MD_BLOCK_CODE` leave handler, no lang detection today).

The five syntax colors live on `BlameUiThemeColors` ([ConfigManager.h:267-280](Source_Core/include/ConfigManager.h:267)) with hard-coded defaults and a `BlameAnalysisConfig::UiColors{}` instance ([line 296](Source_Core/include/ConfigManager.h:296)). They are persisted through [ConfigManager.cpp:1117-1169](Source_Core/src/ConfigManager.cpp:1117). The colors do **not** follow the active theme — switching `Vs2022Dark → Vs2022Light` keeps the same purple keyword + orange string, which is wrong on a light background.

**Goal**: every C++ presentation site uses the syntax tokenizer; the five syntax colors are owned by the active theme; switching theme recolors all sites in the next frame.

---

## Approach

### 1 — Add a syntax-color palette to the theme

**New struct** in [Source_Core/include/SmatchetTheme.h](Source_Core/include/SmatchetTheme.h):

```cpp
struct SmatchetThemeSyntaxColors {
    float Keyword[4];        // {r,g,b,a} 0..1
    float String[4];
    float Comment[4];
    float Number[4];
    float Preprocessor[4];
};
```

**New getter**: `const SmatchetThemeSyntaxColors& SmatchetTheme::GetSyntaxColors();` returns the active theme's syntax palette from a file-static cache filled by `ApplyStyle`.

**Per-theme palettes** in [Source_Core/src/SmatchetTheme.cpp](Source_Core/src/SmatchetTheme.cpp) — each `Apply*` function (`ApplySmatchetDark`, `ApplyModernDark`, `ApplyVs2022Dark`, `ApplyVs2022Light`, `ApplyHighContrast`) writes its quintet to the file-static `gSyntaxColors`. `ApplyStyle` at [line 397-421](Source_Core/src/SmatchetTheme.cpp:397) already routes per theme; no plumbing change.

Hex values per theme (RGB → 0..1 floats in implementation):

| Theme | Keyword | String | Comment | Number | Preprocessor |
|---|---|---|---|---|---|
| SmatchetDark (existing defaults from `BlameUiThemeColors`) | rgb(199,128,255) | rgb(242,166,115) | rgb(115,191,115) | rgb(166,217,255) | rgb(217,217,128) |
| ModernDark | same as SmatchetDark | | | | |
| Vs2022Dark (VS Code Dark+) | rgb(86,156,214) | rgb(206,145,120) | rgb(106,153,85) | rgb(181,206,168) | rgb(155,155,155) |
| Vs2022Light (VS Code Light+) | rgb(0,0,255) | rgb(163,21,21) | rgb(0,128,0) | rgb(9,134,88) | rgb(128,128,128) |
| HighContrast | rgb(255,255,0) | rgb(255,0,255) | rgb(0,255,0) | rgb(0,255,255) | rgb(255,165,0) |

### 2 — Refactor `BlameDrawColoredCppLine` to read the theme directly

**Signature change** in [Source_Core/include/BlameSyntaxHighlight.h](Source_Core/include/BlameSyntaxHighlight.h):

```cpp
void BlameDrawColoredCppLine(const char* utf8Line);                // single line, theme-driven
void BlameDrawColoredCppText(const char* utf8Multiline);           // iterates newline-split; one row per line
```

Both read `SmatchetTheme::GetSyntaxColors()` internally. The seven `theme.SyntaxXxx` references in [BlameSyntaxHighlight.cpp:69, 83, 99, 115, 124, 137, 147](Source_Core/src/BlameSyntaxHighlight.cpp:69) swap to `colors.Xxx` from the getter. Drop the `#include "ConfigManager.h"` from the header; add `#include "SmatchetTheme.h"` in the `.cpp`.

`BlameDrawColoredCppText` splits on `\n`, calls `BlameDrawColoredCppLine` per line, no `SameLine` between lines (each line is its own ImGui row).

### 3 — Trim `BlameUiThemeColors` to non-syntax members; drop syntax config persistence

**Surgical change** to `BlameUiThemeColors` at [ConfigManager.h:267-280](Source_Core/include/ConfigManager.h:267) — keep the seven non-syntax members (`StatusInfo`, `StatusError`, `StatusWarning`, `FindHighlight`, `TextDisabled`, `ImportExisting`, `ClTooltipTitle`) which are used elsewhere in the Blame UI; remove the five `Syntax*` members.

**Config plumbing** in [ConfigManager.cpp:1124-1128](Source_Core/src/ConfigManager.cpp:1124) and [1165-1169](Source_Core/src/ConfigManager.cpp:1165): remove the five `loadRgba("syntax_*", …)` and `putRgba("syntax_*", …)` calls. Legacy `syntax_*` keys in existing `smatchet_config.json` are silently ignored by `loadRgba` (it skips unknown keys). No migration code needed.

**Config schema version bump**: hold until the slice is verified end-to-end per AGENTS.md schema-bump discipline. The bump is one line at `kCurrentBlameAnalysisSchemaVersion` (or the relevant constant) and goes in the **final commit** of the slice.

### 4 — Callstack field opt-in via field-id (cell + tooltip)

**Extend `RenderClippedFieldText`** signature in [Source_Core/include/SmatchetFieldRender.h:8](Source_Core/include/SmatchetFieldRender.h:8):

```cpp
void RenderClippedFieldText(const std::string& rawValue,
                            float availWidth,
                            bool tooltipsEnabled,
                            bool disabled,
                            const std::string* rawForTooltip = nullptr,
                            bool renderMarkdown = false,
                            const std::string* fieldId = nullptr);   // new last param
```

**Body change** at [SmatchetFieldRender.cpp:7-47](Source_Core/src/SmatchetFieldRender.cpp:7): branch on a new helper `IsCallstackFieldId(*fieldId)` that compares against `State().blameCfg.CallstackTrackerFieldId` (already in config at [ConfigManager.h:298](Source_Core/include/ConfigManager.h:298)).

- Callstack branch: inline cell — `BlameDrawColoredCppLine(singleLine.c_str())`; tooltip — `BeginTooltip` → `BlameDrawColoredCppText(tipSource.c_str())` → `EndTooltip` (skip the existing markdown / `PushTextWrapPos` path).
- Default branch: unchanged.

**Call-site updates** — only two grid cells need the new argument because only they render the callstack field:

- [SmatchetActiveProjectGridUi.cpp:916](Source_Core/src/SmatchetActiveProjectGridUi.cpp:916) — pass `&column.FieldId`.
- [TicketFieldEditor.cpp:871](Source_Core/src/TicketFieldEditor.cpp:871) — pass `&column.FieldId`.

Other call sites in [TrackerGridFieldDisplay.cpp:632, 645, 648, 679, 730, 778](Source_Core/src/TrackerGridFieldDisplay.cpp:632) are inside specialized renderers (IssueRestriction, Attachments, etc.) that never carry the callstack value — leave them with the default `nullptr` for `fieldId`.

**Where does `RenderClippedFieldText` get the blame config from?** It needs visibility into `BlameAnalysisConfig::CallstackTrackerFieldId`. Two options:

1. **(Recommended)** Pass the field-id string into a free-function predicate `bool IsCallstackFieldId(const std::string& fieldId)` declared in `SmatchetFieldRender.h`; implementation reads a process-local global `g_callstackFieldId` set once at app startup from `BlameAnalysisConfig::CallstackTrackerFieldId`. Avoids dragging ConfigManager into the renderer's link surface; matches the same indirection used for the existing markdown decision (which is a bool from caller).
2. Caller passes a `bool isCallstackField` already-decided, like the existing `isDescriptionField` at [SmatchetActiveProjectGridUi.cpp:913-915](Source_Core/src/SmatchetActiveProjectGridUi.cpp:913). Slightly more code at each call site; simpler renderer.

Plan picks option 2 — mirror the existing `isDescriptionField` pattern for consistency. Renderer signature becomes:

```cpp
void RenderClippedFieldText(..., bool isCallstackField = false);
```

Caller computes `isCallstackField = !column.FieldId.empty() && column.FieldId == State().blameCfg.CallstackTrackerFieldId;` and passes it.

### 5 — Blame UI raw-callstack read-only view → colored display

[BlameAnalysisUi_Window.cpp:316-323](Source_Core/src/BlameAnalysisUi_Window.cpp:316) is the `if (State().showRaw)` branch — `InputTextMultiline` with `ImGuiInputTextFlags_ReadOnly`. Replace the input widget with `BlameDrawColoredCppText(State().callstackBuf)` while keeping the surrounding `BeginChild("##rawcs_scroll", …)` so the horizontal-scroll geometry is unchanged.

The `else` branch at [line 325-326](Source_Core/src/BlameAnalysisUi_Window.cpp:325) is the editable path (no `ReadOnly` flag) — leave it as `InputTextMultiline`. ImGui input widgets can't be per-token coloured, and users still need to paste/edit.

### 6 — Blame UI Entry tab — drop the `theme` argument

[BlameAnalysisUi_Window.cpp:694](Source_Core/src/BlameAnalysisUi_Window.cpp:694) — `BlameDrawColoredCppLine(ln.Code.c_str(), theme);` becomes `BlameDrawColoredCppLine(ln.Code.c_str());`.

[BlameAnalysisUi_Window.cpp:73](Source_Core/src/BlameAnalysisUi_Window.cpp:73) and [BlameAnalysisUi.cpp:82](Source_Core/src/BlameAnalysisUi.cpp:82) read `State().blameCfg.UiColors` for the non-syntax fields (status / find / tooltip) — these stay; they don't touch syntax members.

### 7 — Markdown — fenced ` ```cpp ` blocks

[MarkdownPreviewRender.cpp:470-473](Source_Core/src/MarkdownPreviewRender.cpp:470) (`MD_BLOCK_CODE` enter) — capture the language tag from md4c's `MD_BLOCK_CODE_DETAIL::lang` (an `MD_ATTRIBUTE`) into a new `std::string PreviewState::codeLang` member. md4c packs `MD_ATTRIBUTE::text` + `MD_ATTRIBUTE::size_or_struct` — copy out `text[0 .. size]` when `lang.text != nullptr`.

[MarkdownPreviewRender.cpp:565-597](Source_Core/src/MarkdownPreviewRender.cpp:565) (`MD_BLOCK_CODE` leave) — on both branches (Tooltip + Full):

- If `s.codeLang ∈ {"cpp","c++","cxx","cc","c","hpp","h","C","C++"}`: keep the existing monospace font push and child-window framing, but iterate `s.codeBuffer` line-by-line and call `BlameDrawColoredCppLine(line)` per line. Drop the hard-coded `PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.6f, 1.0f))` for the cpp path — the tokenizer colors each token; whatever falls through to default text-color picks up the theme's `ImGuiCol_Text`, which is already theme-driven.
- Else (other / no lang): unchanged.

Clear `s.codeLang` alongside `s.codeBuffer.clear()` at [line 594](Source_Core/src/MarkdownPreviewRender.cpp:594).

### 8 — Inline `` `code`` `` spans — **deferred**

Inline `code` spans flow through the styled-run pipeline (`MARK_CODE` at [MarkdownPreviewRender.cpp:40](Source_Core/src/MarkdownPreviewRender.cpp:40), used at [lines 268-280](Source_Core/src/MarkdownPreviewRender.cpp:268) for the bg-tint highlight). Inserting the C++ tokenizer per-word here means cracking open `PreviewRenderRuns` and replacing the inline `TextUnformatted` call with per-token color emit while preserving the rounded-rect bg geometry. The win is marginal — inline spans are typically a single identifier — and the visual change risks breaking the "continuous bar" look users already know.

**Decision**: out of scope for this slice. Filed as a `## Self-improvement` note for the implementing agent to log into `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (category `context`) with rationale + cost estimate.

### 9 — Theme-switch propagation

`SmatchetTheme::ApplyStyle(ThemeId)` runs synchronously when the user switches theme. Each per-theme `Apply*` writes the syntax quintet to `gSyntaxColors` before returning. The next ImGui frame, every colorize call site reads the new palette — no event, no invalidate, no extra signalling.

---

## Files modified

| File | Change |
|---|---|
| [Source_Core/include/SmatchetTheme.h](Source_Core/include/SmatchetTheme.h) | Add `SmatchetThemeSyntaxColors` struct + `GetSyntaxColors()` decl |
| [Source_Core/src/SmatchetTheme.cpp](Source_Core/src/SmatchetTheme.cpp) | Add file-static `gSyntaxColors`; each `Apply*` writes its quintet; `ApplyStyle` already routes; add `GetSyntaxColors()` body |
| [Source_Core/include/BlameSyntaxHighlight.h](Source_Core/include/BlameSyntaxHighlight.h) | New no-arg `BlameDrawColoredCppLine`; new `BlameDrawColoredCppText`; drop `ConfigManager.h` include |
| [Source_Core/src/BlameSyntaxHighlight.cpp](Source_Core/src/BlameSyntaxHighlight.cpp) | Drop `BlameUiThemeColors` param; pull colors from `SmatchetTheme::GetSyntaxColors()`; add multiline helper; include `SmatchetTheme.h` |
| [Source_Core/include/ConfigManager.h](Source_Core/include/ConfigManager.h) | Trim 5 `Syntax*` members from `BlameUiThemeColors` (keep 7 non-syntax members) |
| [Source_Core/src/ConfigManager.cpp](Source_Core/src/ConfigManager.cpp) | Drop 5 `syntax_*` `loadRgba`/`putRgba` calls; bump config schema version (final commit) |
| [Source_Core/include/SmatchetFieldRender.h](Source_Core/include/SmatchetFieldRender.h) | `RenderClippedFieldText` gains optional `bool isCallstackField = false` |
| [Source_Core/src/SmatchetFieldRender.cpp](Source_Core/src/SmatchetFieldRender.cpp) | Branch on `isCallstackField`; use `BlameDrawColoredCppLine` / `…Text` |
| [Source_Core/src/SmatchetActiveProjectGridUi.cpp](Source_Core/src/SmatchetActiveProjectGridUi.cpp) | Compute `isCallstackField` from `column.FieldId == State().blameCfg.CallstackTrackerFieldId`; pass to renderer |
| [Source_Core/src/TicketFieldEditor.cpp](Source_Core/src/TicketFieldEditor.cpp) | Same as above |
| [Source_Core/src/BlameAnalysisUi_Window.cpp](Source_Core/src/BlameAnalysisUi_Window.cpp) | Replace `showRaw=true` `InputTextMultiline` with `BlameDrawColoredCppText` inside the existing child; drop `theme` arg at line 694 |
| [Source_Core/src/MarkdownPreviewRender.cpp](Source_Core/src/MarkdownPreviewRender.cpp) | Capture `MD_BLOCK_CODE_DETAIL::lang` on enter; per-line colorize fenced cpp on leave; clear `codeLang` alongside `codeBuffer` |

**Reused — do not rewrite**:

- Keyword table at [BlameSyntaxHighlight.cpp:16-34](Source_Core/src/BlameSyntaxHighlight.cpp:16) — 120+ entries, single source of truth.
- Tokenizer state machine at [BlameSyntaxHighlight.cpp:66-154](Source_Core/src/BlameSyntaxHighlight.cpp:66) — comments, strings, numbers, preprocessor, identifiers, operators.
- md4c integration at [MarkdownPreviewRender.cpp:470-597](Source_Core/src/MarkdownPreviewRender.cpp:470) — only the enter/leave `MD_BLOCK_CODE` path gains lang awareness.
- `BlameUiThemeColors` non-syntax members ([ConfigManager.h:268-274](Source_Core/include/ConfigManager.h:268)) — used by `BlameAnalysisUi.cpp` / `BlameAnalysisUi_Window.cpp` for status / find / tooltip — untouched.
- `BlameAnalysisConfig::CallstackTrackerFieldId` at [ConfigManager.h:298](Source_Core/include/ConfigManager.h:298) — already populated by user config; the new branch reads it.

---

## Subsystem split / agent dispatch

Scope spans `p4-blame` (BlameSyntaxHighlight, BlameAnalysisUi) + `grid-engine` (SmatchetFieldRender, SmatchetActiveProjectGridUi, TicketFieldEditor) + non-row files (SmatchetTheme, MarkdownPreviewRender, ConfigManager).

Two-packet delegation (sequential — second packet depends on first):

1. **Packet 1 — theme + tokenizer plumbing** (orchestrator or `p4-blame`): SmatchetTheme + BlameSyntaxHighlight + ConfigManager trim + BlameAnalysisUi_Window line 694 + showRaw replacement. Lands first; everything else compiles against the new signature.
2. **Packet 2 — call-site wiring** (`grid-engine` + MarkdownPreviewRender owner): SmatchetFieldRender param + the two grid call sites + MarkdownPreviewRender lang-tag detection. Lands after packet 1.

Final commit of packet 2 carries the config schema version bump.

---

## Verification

Build gate (mandatory):

```
cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12
cmake --build --preset ninja-test-msys2
```

Doctest unit coverage to add under `tests/Source_Core/`:

- `BlameSyntaxHighlight.test.cpp` — exercise the no-arg overload + multiline helper against a fixed `gSyntaxColors` (test sets it directly). Verify token boundaries for keyword / string / number / comment / preprocessor on representative C++ lines.

Functional sweep (target = **zero manual** via `test-author` after first-round implementation):

1. `SmatchetStandalone.exe` — launch.
2. Open Blame UI, load a ticket whose `CallstackTrackerFieldId` value is populated, Entry tab — confirm keyword / string / comment / number / preprocessor colored.
3. Settings → Theme → cycle `SmatchetDark → ModernDark → Vs2022Dark → Vs2022Light → HighContrast`. Each switch recolors the visible code in the same frame without restart.
4. Active project grid — hover a row whose callstack field is multi-line — tooltip pops with colored output. The cell itself (single-line clip) is colored.
5. Ticket detail — description with ` ```cpp\n…\n``` ` fenced block — preview shows colored. Same with ` ```c++ `.
6. Description with ` ```python ` fenced block — still renders monospace flat-color (no false positive).
7. Blame UI raw-callstack panel with `showRaw=true` — display is read-only colored. Parse / Paste-from-clipboard buttons still populate the buffer. `showRaw=false` path remains editable plain text (intentional).
8. Pillar 1 gate — `perf-measure` on `blame_open_entry_tab` scenario before vs. after — mean frame ≤ 6.94 ms. The tokenizer is hot for big callstacks; regression here is a blocker.
9. Pillar 3 gate — `ninja-test-msys2` ASan / UBSan pass.

After first verification round: `test-author` converts steps 2-7 into deterministic scenario + screenshot-diff assertions per AGENTS.md § Verification automation.

Plan revision contract: append `## Implementation log` + `## Deviations from plan` + `## Verification` sections to this doc in the same or next commit per AGENTS.md § Plan revision after implementation.

---

## Open questions

None — user resolved scope (all sites + markdown fenced block), color source (theme-driven, drop user override on syntax members only), API shape (`GetSyntaxColors()` getter).

---

## Implementation log

- Source_Core/include/SmatchetTheme.h · added `SmatchetThemeSyntaxColors` struct + `SmatchetTheme::GetSyntaxColors()` decl.
- Source_Core/src/SmatchetTheme.cpp · file-static `gSyntaxColors` + `SetSyntaxColors`; each of `ApplySmatchetDark` / `ApplyModernDark` / `ApplyVs2022Dark` / `ApplyVs2022Light` / `ApplyHighContrast` writes its quintet; `GetSyntaxColors()` returns the cache.
- Source_Core/include/BlameSyntaxHighlight.h · dropped `#include "ConfigManager.h"`; new no-arg `BlameDrawColoredCppLine`; new `BlameDrawColoredCppText` multiline helper.
- Source_Core/src/BlameSyntaxHighlight.cpp · reads `SmatchetTheme::GetSyntaxColors()`; multiline helper splits on `\n`; incidental fix: removed dead-store `i = n;` before `break` in `//` comment branch.
- Source_Core/include/ConfigManager.h · trimmed 5 `Syntax*` members from `BlameUiThemeColors`; doc comment now points at `SmatchetTheme::GetSyntaxColors()`.
- Source_Core/src/ConfigManager.cpp · dropped 5 `loadRgba("syntax_*"…)` and 5 `putRgba("syntax_*"…)` calls; incidental fix: removed redundant default assignment in `UiDensity` switch (clang-analyzer dead-store).
- Source_Core/include/SmatchetFieldRender.h · `RenderClippedFieldText` gains optional `const std::string* fieldId`; new `SetCallstackFieldIdHint`.
- Source_Core/src/SmatchetFieldRender.cpp · file-static `g_callstackFieldId`; renderer branches on `IsCallstackField` → `BlameDrawColoredCppLine` for inline cell, `BlameDrawColoredCppText` for tooltip; default path unchanged.
- Source_Core/src/SmatchetActiveProjectGridUi.cpp:916 · passes `&column.FieldId`.
- Source_Core/src/TicketFieldEditor.cpp:871 · passes `&column.FieldId`.
- Source_Core/src/BlameAnalysisUi_Config.cpp · `HydrateBlameCfgDiskOnce` + `MaybeAutoselectCallstackTrackerField` call `SetCallstackFieldIdHint` so the renderer hint stays in sync with config; added `SmatchetFieldRender.h` include.
- Source_Core/src/BlameAnalysisUi_Preferences.cpp · `PersistBlameCfg` calls `SetCallstackFieldIdHint` after every preference save; added `SmatchetFieldRender.h` include.
- Source_Core/src/BlameAnalysisUi_Window.cpp · line 694 drops `theme` arg from `BlameDrawColoredCppLine`; the `showRaw=true` branch (lines 316-327) replaced `InputTextMultiline(ReadOnly)` with `BlameDrawColoredCppText(callstackBuf)` inside the existing `BeginChild("##rawcs_scroll", …)`; rawFieldW / rawMaxLineW scoped inside the branch to silence pre-existing cppcheck variableScope hint.
- Source_Core/src/MarkdownPreviewRender.cpp · `PreviewState` gains `codeLang`; `MD_BLOCK_CODE` enter captures `MD_BLOCK_CODE_DETAIL::lang`; leave does per-line `BlameDrawColoredCppText` for `{cpp,c++,cxx,cc,c,hpp,h}` fences on both Tooltip + Full paths, otherwise unchanged; `codeLang` cleared alongside `codeBuffer`; added `BlameSyntaxHighlight.h` + `StringUtil.h` includes.
- .clang-tidy · extended `ExcludeHeaderFilterRegex` to also skip `.fetchcontent-src` / `.fetchcontent-build` trees (vendored ImGui headers were tripping `clang-analyzer-deadcode.DeadStores` warnings on any TU that includes them).

## Deviations from plan

- **`RenderClippedFieldText` callstack opt-in** — plan §4 picked option 2 (caller computes `bool isCallstackField` like the existing `isDescriptionField` pattern), but `BlameAnalysisConfig::CallstackTrackerFieldId` is not reachable from `SmatchetActiveProjectGridUi.cpp` / `TicketFieldEditor.cpp` (`d.cfg` is `TrackerConfig`, not `BlameAnalysisConfig`; the blame config lives on `BlameAnalysisUi::State()`). Switched to option 1 — process-local global hint set by `SetCallstackFieldIdHint`, called from `BlameAnalysisUi_Config.cpp` (load + auto-select) and `BlameAnalysisUi_Preferences.cpp` (edit / persist). Renderer reads the cached id via internal `IsCallstackField`. Callers pass `&column.FieldId` only.
- **Config schema version bump** — held / skipped. Legacy `syntax_*` keys in existing `smatchet_config.json` are silently dropped by `loadRgba` (it ignores unknown keys); the on-disk format remains forward-compatible without a bump. No user-visible migration friction.
- **Doctest unit coverage for `BlameDrawColoredCppLine`** — deferred. The tokenizer requires an active ImGui context to emit `TextUnformatted` / `PushStyleColor`; a true pure-logic test would have to extract the tokenizer state-machine into a context-free helper first. Filed for a follow-up `test-rig` slice once the refactor extracts the token boundary computation from the draw call.
- **Inline `` `code`` `` spans** — confirmed out of scope per plan §8.

## Verification

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone` → **pass** (exit 0; 84/84 targets).
- `cmake --build --preset ninja-iter-msys2 --target SmatchetCore_DX12` → **pass** (exit 0; 77/77 targets).
- `cmake --build --preset ninja-test-msys2` → **pass** (exit 0).
- `ctest` in `build/ninja-test-msys2/` → **pass** (1/1 smatchet_tests).
- Lint hook (clang-format / cppcheck / clang-tidy) → **clean** on every touched first-party `.cpp` / `.h`.

Manual residue (still requires a UI session — flag to `test-author` next):

- Visual confirm: open Blame UI Entry tab, switch theme `SmatchetDark → Vs2022Dark → Vs2022Light → HighContrast`, expect annotated source lines re-color in the next frame.
- Visual confirm: active-project grid → hover row whose callstack field is multi-line → tooltip is colored.
- Visual confirm: ticket detail with ` ```cpp\n…\n``` ` markdown fence → preview is colored; ` ```python ` fence stays plain.
- Visual confirm: Blame UI raw-callstack toggle `showRaw=true` → text is colored, read-only; `showRaw=false` → still editable plain `InputTextMultiline`.
- Perf gate: `perf-measure` on `blame_open_entry_tab` scenario before/after — mean frame ≤ 6.94 ms (tokenizer is hot path for big callstacks).

## Follow-up — Blame→generic rename (same slice)

User requested stripping the `Blame` prefix once the tokenizer became multi-consumer (callstack field, markdown fence, blame raw view, blame entry tab).

- Files renamed: `Source_Core/include/BlameSyntaxHighlight.h` → `CppSyntaxHighlight.h`; `Source_Core/src/BlameSyntaxHighlight.cpp` → `CppSyntaxHighlight.cpp`.
- Symbols renamed: `BlameDrawColoredCppLine` → `DrawColoredCppLine`; `BlameDrawColoredCppText` → `DrawColoredCppText`.
- Header guard: `BLAME_SYNTAX_HIGHLIGHT_H` → `CPP_SYNTAX_HIGHLIGHT_H`.
- CMake: `CMakeLists.txt` var `_SMATCHET_BLAME_SYNTAX` → `_SMATCHET_CPP_SYNTAX`; conditional path updated.
- Callers updated: `BlameAnalysisUi_Window.cpp`, `MarkdownPreviewRender.cpp`, `SmatchetFieldRender.cpp` (include + call sites).
- Doc comments: `SmatchetTheme.h`, `SmatchetTheme.cpp`, `SmatchetFieldRender.h`.
- Agent docs: `AGENTS.md`, `agents/p4-blame.md` (canonical); `.claude/agents/p4-blame.md` regenerated via `bash scripts/sync-agents.sh`.
- Backlog historical mention: `backlog/POST_P0_REVIEW.md`.
- Build gate post-rename: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` → **pass** (exit 0; 35/35).
- Grep verification: zero remaining `BlameSyntaxHighlight` / `BlameDrawColored*` refs in the working tree.

## Shipped PRs

| PR | Title | Squash sha on develop |
|---|---|---|
| [#80](https://github.com/alexandrosk0/Smatchet/pull/80) | `feat(theme): per-theme C++ syntax palette + generic CppSyntaxHighlight` | `2e783d6` |
| [#81](https://github.com/alexandrosk0/Smatchet/pull/81) | `test(theme-residue): bucket-A markdown lang-tag doctest + backlog entries for items #3, #4` | `6e3e6e8` |

## Residue still in backlog

Filed under `docs/backlog/AGENT_SELF_IMPROVEMENT.md` (2026-05-15 · test-author · [tooling]):

- Markdown rendered-output coverage for ` ```cpp ` fence — bucket-B scenario + bucket-C screenshot diff. ~30 min once parent harness lands.
- Blame UI raw-callstack `showRaw=true` colored-display verification — bucket-E ImGui Test Engine fixture; needs first Blame UI fixture under `tests/ui/`. ~2 h.
- `perf-measure` scenario `blame_open_entry_tab` for Pillar 1 regression gate — needs callstack-injection API on `AppController` + ~100 LoC scenario class + `scripts/dev/test-blame-perf.sh` runner. ~3 h.

