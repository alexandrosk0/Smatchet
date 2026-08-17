# Plan — GitHub issue Body cell looks empty when the body opens with a blank line
<!-- plan-date: 2026-08-16 -->

> **Slug**: `github-issue-body-empty-line`
>
> **Status**: `shipped`

## Context

A GitHub issue whose Body has full text often renders as an **empty grid cell** in Smatchet. User report: *"Often in the Body of issues in github looks empty, while it's has a full text. Don't let the first line be empty."*

Cause is display-side, in the single choke point every one-line field cell goes through: [`RenderClippedFieldText`](../../../Source/Core/src/Ui/SmatchetFieldRender.cpp:30) truncates the value at the **first** `\r`/`\n`. When the value *starts* with a newline — the common shape for GitHub issue bodies (a leading blank line before the first heading), Jira ADF→markdown conversions, and pasted descriptions — `find_first_of` returns `0`, `singleLine` becomes `""`, and the cell draws nothing. The tooltip already shows the full raw text, which is why the ticket "has full text" while the cell is blank.

After this lands: a one-line cell previews the first line **with visible content**, so a body that opens with blank lines is never rendered as an empty cell.

## Approach

Replace "first line" with "first line carrying visible content" at the one choke point. A new header-only pure helper `smatchet::field_preview::FirstVisibleLine(value)` walks lines from the start, skipping blank / whitespace-only ones, and returns the first line with a non-space character plus a `HasMoreLines` flag. `SmatchetFieldRender.cpp` calls it in place of the inline `erase(pos)` block.

Fix is **display-side, not data-side** on purpose. Stripping the leading blanks in [`GitHubIssueSearchMapping.cpp:190`](../../../Source/Core/src/Tracker/GitHubIssueSearchMapping.cpp:190) would mutate the stored field value and corrupt write-back on edit — the round-trip must preserve the body byte-for-byte. The tooltip keeps showing the untouched raw value.

`HasMoreLines` preserves the existing `hasNewline` semantics exactly (`value.find_first_of("\r\n") != npos`), so the overflow-tooltip trigger is unchanged; only the *drawn* line moves. The chosen line is returned verbatim (no trim), so an intentional indent still shows. CRLF counts as one break, not a blank line.

The helper is header-only and ImGui-free so bucket-A doctest can link it (`test-rig` refuses UI/ImGui surfaces) — same pattern as [`AsyncLoadGatePure.h`](../../../Source/Core/include/Ui/AsyncLoadGatePure.h:1).

## Files to modify

1. **`Source/Core/include/Ui/FieldPreviewLinePure.h`** (new) — pure `FirstVisibleLine()` + `PreviewLine{Text, HasMoreLines}`. Grepped `Source/Core/` for `FirstVisibleLine` / `PreviewLine` / `FieldPreview` — no existing TU under that name or a synonym.
2. **[`Source/Core/src/Ui/SmatchetFieldRender.cpp:30`](../../../Source/Core/src/Ui/SmatchetFieldRender.cpp:30)** — include the new header; replace the inline first-newline truncation with the helper call.
3. **`tests/Core/FieldPreviewLinePure.test.cpp`** (new) — bucket-A doctest coverage of the line-picking rules.
4. **[`tests/CMakeLists.txt`](../../../tests/CMakeLists.txt:1129)** — register the new test TU (header-only helper → no production `.cpp` link line).
5. **`docs/plans/github-issue-body-empty-line.md`** — this plan.

No `SMATCHET_WITH_*` source-list gating is touched.

## Existing utilities reused

- `RenderClippedFieldText` — [`Source/Core/src/Ui/SmatchetFieldRender.cpp:25`](../../../Source/Core/src/Ui/SmatchetFieldRender.cpp:25) — the single choke point for every one-line cell preview (description cells, generic field cells, the saving-state cell, the mobile detail pane, all `TrackerGridFieldDisplay` rows); fixing it here covers every backend without touching any caller.
- `AsyncLoadGatePure.h` — [`Source/Core/include/Ui/AsyncLoadGatePure.h:1`](../../../Source/Core/include/Ui/AsyncLoadGatePure.h:1) — house pattern for a header-only, ImGui-free `*Pure.h` helper unit-tested by bucket-A; the new header follows its shape (file-header WHY block, `namespace smatchet { namespace <topic> {`, `inline`, `///` docs).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — extracts ~7 lines of inline logic into a new pure helper for testability; no over-cap file is being split, and `SmatchetFieldRender.cpp` (91 lines) is nowhere near the TU ceiling.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — same single pass over the value's leading bytes as the existing `find_first_of`, executed once per drawn cell. The loop only advances across leading *blank* lines (typically zero or one), then returns.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — pure in-memory string work, no I/O, no locks, no allocation beyond the one `substr` the old code already did via `erase`.
- **Pillar 3 (never crash)**: no impact — all indexing is bounds-checked against `value.size()`; the CRLF look-ahead guards `start < value.size()` before `value[start]`. The empty-string and all-blank cases return early with an empty `Text`, matching today's behaviour. Bucket-A tests pin every boundary case.
- **Pillar 4 (accessibility)**: mild improvement — a cell that previously rendered as blank now shows readable text, so screen-reader / low-vision users get content where there was none. No keyboard-nav or contrast surface touched.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Diff touches `Source/Core/` → gates declared:

1. **PR-fast CI** — grid-scroll / ticket-grid render scenario is the one exercising `RenderClippedFieldText` (per-cell draw path). Map: `agents/core/perf-gatekeeper.md` § Curated diff → scenario map.
2. **Pillar 2 static scanner** — N/A: no new sync I/O; the helper is pure string logic with no filesystem / network / SQLite call.
3. **Dispatcher drain** — N/A: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — N/A: adds no sync-stall path > 100 ms.
5. **Marker inventory** — N/A: adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: per-cell string scan on an already-hot draw path; the change is O(leading blank lines) on top of an existing O(first line) scan, so a baseline comparison is run against the grid scenario before opening the PR.

**Override**: none requested.

## Risks / non-goals

- **Risk — a cell now shows a line that isn't line 1.** For a value like `"\n\nHello"` the cell shows `Hello` rather than nothing. That is the requested behaviour; the tooltip still shows the untouched raw value, so nothing is hidden. Accepted.
- **Risk — callstack fields.** `DrawColoredCallstackLine` receives the picked line instead of the literal first line. A callstack that opens with a blank line previously drew an empty cell; it now draws the first real frame — strictly better, and the callstack tooltip already shows the full text unconditionally. Accepted.
- **Risk — whitespace-only content is treated as blank.** A line of only spaces/tabs is skipped. It renders as nothing anyway, so skipping it is what the user asked for. Accepted.
- **Non-goal — data-side normalisation.** The stored field value is NOT trimmed; write-back round-trips the body byte-for-byte.
- **Non-goal — multi-line cell previews.** Cells stay one line; this plan does not add wrapping or a two-line preview.
- **Non-goal — the bug-report body builder.** [`BugReportBody.cpp`](../../../Source/Core/src/Diagnostics/BugReportBody.cpp:160) already opens with `## Bug report\n\n` and already falls back on an empty first line; ruled out as a cause and left alone.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `tests/Core/FieldPreviewLinePure.test.cpp` — empty value; single line no newline; `"a\nb"`; leading `\n`; leading whitespace-only line; CRLF (`"\r\nHello"` and `"a\r\nb"`); all-blank value; trailing newline; indent preserved on the chosen line. Asserts both `Text` and `HasMoreLines`.
- **Bucket E (ImGui Test Engine)**: N/A — the drawn-line decision is fully covered by bucket A, and no new ImGui widget / interaction is introduced.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no new code path reachable from the CLI; the render change is a one-line text substitution inside an existing widget call.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run before finalising; record the outcome in § Verification (actual).
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — nothing is being deferred from an existing shipped scope, so no stale "deferred-as-current" references exist to sweep.

- **Trimming trailing blank lines** — irrelevant to a one-line preview; no action.
- **Rendering markdown in the cell itself** — cells stay plain text; the markdown tooltip path is unchanged. No action.
- **Normalising bodies at the tracker-mapping layer** — deliberately rejected (would corrupt write-back). No follow-up plan.

## Implementation log

Shipped exactly as planned, 4 files + this plan:

1. **`Source/Core/include/Ui/FieldPreviewLinePure.h`** (new, 64 lines) — `PreviewLine{Text, HasMoreLines}` + `inline FirstVisibleLine(const std::string&)`. Walks lines from the start, skips blank / whitespace-only ones (`find_first_not_of(" \t")`), returns the first line with content verbatim. `HasMoreLines` is computed up front as `value.find_first_of("\r\n") != npos` — the old `hasNewline` expression byte-for-byte. CRLF is consumed as one break via a bounds-guarded look-ahead.
2. **[`Source/Core/src/Ui/SmatchetFieldRender.cpp:31`](../../../Source/Core/src/Ui/SmatchetFieldRender.cpp:31)** — the 7-line inline `erase(pos)` block replaced by the helper call; `singleLine` is now a `const std::string&` bound to `preview.Text`. Every downstream use (`CalcTextSize`, `DrawColoredCallstackLine` / `TextUnformatted`, the tooltip trigger) is unchanged.
3. **`tests/Core/FieldPreviewLinePure.test.cpp`** (new) — 5 `TEST_CASE`s / 22 assertions.
4. **[`tests/CMakeLists.txt`](../../../tests/CMakeLists.txt)** — TU registered after `Core/AsyncLoadGatePure.test.cpp`; header-only helper, so no production `.cpp` link line.

No caller was touched: fixing the single choke point covered the description cell, generic field cells, the saving-state cell, the mobile detail pane and every `TrackerGridFieldDisplay` row across all backends.

## Deviations from plan

- **`scripts/dev/test-docs.sh` is not fully green on Windows — `test-gate-selftests` fails, pre-existing and unrelated to this diff.** This worktree's `agents/scripts/core/test-gate-selftests.sh` is byte-identical to `origin/develop`'s and is not in this PR's diff. Cause: MSYS / Git-Bash reports any file whose first bytes are `#!` as executable, so the gate's untracked-fixture fallback (`[ -x "$f" ]`, used because synth fixtures are untracked) classifies every `#!/usr/bin/env bash` fixture as mode-100755 and skips the raw-self-exec rule — its six `--selftest` negatives cannot fire on Windows. Verified directly: a two-line `#!`-headed file reports `-x` true in this shell, a shebang-less one false. The production `--check` path is unaffected (tracked files take their mode from the git index). Fixing a shared gate was judged out of scope for a UI display fix; filed to the backlog instead (`docs/self-improvement/categories/tooling/2026-08-16-gate-selftests-msys-exec-bit.md`). The other 18 `test-docs.sh` checks pass.
- **Plan-doc source links were repo-root-relative on first write and failed `test-markdown-links.sh`.** Rewritten to the house `../../../` doc-relative form; the checker is green.
- **`grill-with-docs` was not run.** The plan's own § Verification bullet asked for it; the harness exposes no such skill in this session. The stress it stands in for was covered instead by the bucket-A boundary cases (empty / all-blank / CRLF / trailing-newline / indent-preserved) plus the explicit `HasMoreLines`-semantics-unchanged assertions.

## Verification (actual)

- **Bucket A** — `tests/Core/FieldPreviewLinePure.test.cpp`: `5 passed / 0 failed`, `22 assertions passed / 0 failed`.
- **Full ctest** (`ctest --preset ninja-test-msvc`): `100% tests passed, 0 tests failed out of 7` (33.9 s).
- **Build gate** (`cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`): clean — `SmatchetCore_DX12.lib` and `Smatchet.exe` both linked, 1021/1021 targets, zero warnings-as-errors.
- **Lint gates** (`agents/scripts/project/test-lint-rules.sh --diff origin/develop`): all 12 reported rules PASS, including strict-zone, comment-noise, oversized-functions, include-cycle and the `AppController.h` fan-in ratchet. `clang-format -i` applied to all three touched C++ files.
- **Doc validation** (`scripts/dev/test-docs.sh`): 18 of 19 checks pass; `test-gate-selftests` red for the pre-existing Windows reason recorded under § Deviations. `test-markdown-links.sh` and `test-agent-contract.sh` both green.
- **Manual residue**: none — the drawn-line decision is fully covered by bucket A.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
