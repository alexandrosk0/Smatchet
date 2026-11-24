# Plan — Input UX Wave 2 leftover: the text-field and matcher surfaces the locks held back
<!-- plan-date: 2026-11-24 -->

> **Slug**: `input-ux-w2-surface-leftovers` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Two Wave 2 pieces stopped at files that other branches' plan-locks held when they shipped:

- The shared text field (`docs/plans/shipped/input-ux-w2-text-field.md`, PR #2438). Its leftovers are listed in `debt/2026-11-16-text-field-migration-blocked-surfaces.md`.
- The shared matcher (`docs/plans/shipped/input-ux-w2-one-matcher.md`, PR #2456). Its leftovers are listed in `debt/2026-11-19-one-matcher-deferred.md`.

Three of those surfaces are now in files that no live lock or open PR touches:

- **#125 (audit):** the toolbar editor gives no feedback when a typed icon name matches no icon. The button just shows no glyph.
- **Toolbar command list:** it ranks through the matcher but does not mark the matched letters, which the dropdown and the palette do.
- **#131 (audit):** `ApplyPathRemaps` now trims and folds. The Annotate preferences still do not trim the stored prefixes on commit, and they have no Preview field showing what a sample path becomes.

Two debt bullets are already done: the labels editor's Enter adds a label and keeps the caret (#4), and the filter focus in the project picker and labels popup (#3). The shared dropdown (#2432) does both:

- `FilterDropdown.cpp` `FinishEvent` clears the filter after an Add and keeps the caret in a tick-box list.
- `tests/ui/filter_dropdown.test.cpp` cases 1, 3, 6, A, C and R cover it.

This plan removes their bullets.

Intended outcome:

- A mistyped icon name says so.
- The toolbar command list marks why each row matched.
- A remap rule can be checked against a sample path before a crash report needs it.

## Approach

- **Icon name (#125).** Under the icon row, a `TextDisabled` line "No such icon. Pick... lists them." shows when the name is not blank and resolves to no glyph. A pure helper in `SmatchetToolbarUi_detail.h` (`IconNameMatchesNoIcon`) makes the decision.
- **Toolbar command marks.**
  - When the filter changes, the editor computes each listed row's matched byte ranges in its name, at the same time as the ranking.
  - A pure helper, `CommandNameMatchRanges`, runs `textmatch::MatchFields` on the same haystack `RankCommandMatches` ranks with (name plus title). It uses one `textmatch::Scratch` member and keeps only the ranges that fall inside the name.
  - The row stays `ImGui::Selectable(name)`, so its ID and the existing test refs are unchanged. Its own text is drawn transparent, and `SmatchetMatchHighlight::DrawText` draws the name with the marks on top.
  - An empty filter marks nothing.
- **Remap trim and Preview (#131).**
  - The from/to fields use `SettingFieldOptions(true)`, so edge whitespace is trimmed on commit. `ApplyPathRemaps` already ignores it, so what is stored now matches what applies.
  - Under the rules, a "Preview" field takes a sample path. The sample lives in the editor state and is not saved.
  - Beside it, a line shows the path `ApplyPathRemaps` makes of the sample with the current rules. When the path comes back unchanged, the line says "No rule changes this path."

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage change. This adds:

- UI-internal pure helpers in `SmatchetToolbarUi_detail.h`.
- Three `extern "C"` test hooks in `AnnotateAnalysisUi_Preferences.cpp`, beside the existing remap hooks.

## Files to modify

1. `Source/Core/src/Ui/SmatchetToolbarUi.cpp`, `Source/Core/include/Ui/SmatchetToolbarUi.h`, `Source/Core/src/Ui/SmatchetToolbarUi_detail.h`: the icon line, the command marks (range cache and scratch members), the two helpers.
2. `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`, `Source/Core/src/Ui/AnnotateAnalysisUi_Internal.h`: the trim, the Preview field and its state, the test hooks.
3. `tests/Core/ToolbarEditorPure.test.cpp`: doctests for both helpers.
4. `tests/ui/funcsize_main_ui_smoke.test.cpp`: the command list draws the mark colour for a filter and none without one; an unknown icon name leaves the glyph empty.
5. `tests/ui/gesture_keys.test.cpp`: a typed rule with edge spaces is stored trimmed; Preview shows the rewritten path and the no-match line.
6. `docs/plans/shipped/input-ux-w2-surface-leftovers.md` (this plan, moved on ship), `docs/plans/INDEX.md`, and the two debt entries above.

## Existing utilities reused

- `textmatch::MatchFields`, `textmatch::Scratch`, `SmatchetMatchHighlight::DrawText`, `RankCommandMatches`.
- `SmatchetToolbarIconGlyph`, `ApplyPathRemaps`, `SmatchetInput::Text` with `SettingFieldOptions`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. Well under 150 lines are added.

## UX Pillar callouts

- **Pillar 1**: the command ranges are computed once per filter change, together with the ranking, not every frame. The draw adds one text call per visible row. The Preview runs `ApplyPathRemaps` once per frame over a handful of rules, only while the Preferences page is open.
- **Pillar 2**: no I/O. The sample path is not persisted.
- **Pillar 3**: ranges are clipped to the name, and `DrawText` clips stale ranges.
- **Pillar 6**: N/A, no tracker data.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires. No perf scenario opens the toolbar editor or the Annotate preferences.
2. **Pillar 2 static scanner**: N/A, no sync I/O added.
3. **Dispatcher drain**: N/A, not touched.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: the marks misalign with the row text.** The name is drawn at the Selectable's own text position (item rect min, the default `SelectableTextAlign`), in the same font.
- **Non-goal**: the other bullets of the two debt entries. Each stays out for one of these reasons:
  - **Files still locked**:
    - `SmatchetUiSession.h` and `SmatchetLocalization.cpp`, by `bulk-status-allowed-transitions`.
    - `tests/CMakeLists.txt` and the UI test registry, by `input-ux-w3-grid-s2`.
  - **Wave 3 owns it**:
    - The grid surfaces, `input-ux-w3-grid-spreadsheet` S3.
    - The grid "close matches" cue, `input-ux-w3-filter-without-jql` S7.
  - **Needs its own design**:
    - The suggestion popup for more fields (#10).
    - Label suggestions from the tracker (#42).
  - **Needs a human**: the palette golden.

## Implementation handoff

N/A: single slice in `Source/Core/src/Ui/`.

## Verification

- **Bucket A** (`ToolbarEditorPure.test.cpp`):
  - `IconNameMatchesNoIcon`: blank, known and unknown names.
  - `CommandNameMatchRanges`: a word as written, letters in order, a word that occurs only in the title (marks nothing in the name), an empty query.
- **Bucket E**:
  - `FuncSizeMainUi/ToolbarEditor_CommandFilterIsFuzzy` gains the mark check: a filter puts `ImGuiCol_TextSelectedBg` vertices in `##cmdlist`, and an empty filter puts none.
  - A new step types an unknown icon name and checks that the glyph stays empty.
  - `GestureKeys/PathRemaps_AddFocusesAndEnterAddsNext` gains the trim and Preview steps.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`; unit `ninja-test-msvc`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (item 5 of the Wave 2 leftovers). The terms (marks, matcher, remap rule) follow the two shipped plans.
- **Manual residue**: none planned.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Everything listed under Non-goal above stays in the two debt entries.

## Implementation log

## Deviations from plan

## Verification (actual)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
