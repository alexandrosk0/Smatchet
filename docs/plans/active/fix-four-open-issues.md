# Plan — fix four open GitHub Issues (#2109 / #2093 / #2079 / #2066)

> **Slug**: `fix-four-open-issues` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Four open product Issues, all small and independent, batched onto one branch per
`AGENTS.md` § Autonomous ship-loop default (one PR per logical feature — here, one
"open-issue sweep"). #2099 is explicitly out of scope (user instruction).

- **#2109** — "Body in github issues and PRs look empty when now": a GitHub body that
  opens with a blank line renders an empty one-line cell in the *editable* field cell.
  The read-only grid path already got the fix (#2097 → `FirstVisibleLine`); the
  `TicketFieldEditor` text-editor cell still cuts at the first `\n`/`\r`.
- **#2093** — `SmatchetAboutUi.cpp` calls `IsPopupOpen(const char*, ImGuiPopupFlags_AnyPopupLevel)`,
  which ImGui explicitly asserts on (`imgui.cpp` — "Cannot use IsPopupOpen() with a string id
  and ImGuiPopupFlags_AnyPopupLevel"). Debug builds fire the assert on every About dismiss.
- **#2079** — `CompareFieldValuesForSort` is intransitive on a column mixing parseable and
  unparseable values (`"5x" < "9" < "10" < "5x"`), feeding `std::stable_sort` → UB (Pillar 3).
- **#2066** — the `"unknown"` placeholder in the About modal bypasses the localization
  dictionary, so it stays English under `fr-FR`.

After this lands: no ImGui assert on About dismissal, the sort comparator is a strict weak
ordering for every input, blank-leading bodies preview their first visible line in the editor
cell, and the About placeholder translates.

## Approach

Four independent, minimal fixes — no shared refactor, no new TU.

1. **#2093**: include `imgui_internal.h` (before the `#define ImGui SmatchetLocalizedImGui`)
   and pass the pre-hashed id: `::ImGui::IsPopupOpen(::ImGui::GetID(kAboutPopupId), ImGuiPopupFlags_AnyPopupLevel)`.
   That is byte-for-byte what the asserting overload computes internally (same `g.CurrentWindow->GetID`
   seed), minus the assert. The raw English literal is the correct hash source even under `fr-FR`:
   `WindowTitleFromSource` appends `###About Smatchet`, and `ImHashStr` resets `crc = seed` at `###`
   *and skips the three `#`* (`data += 2; continue;`), so both hash identically.
2. **#2066**: route the placeholder through `SmatchetLocalization::TranslateSource("unknown")`
   and add the dictionary row. Chrome translates; the DATA values stay verbatim (existing About rule).
3. **#2079**: make the sort key lexicographic on `(parses-as-number ? 0 : 1, numeric value, raw string)`.
   Category is the primary key, so a numeric cell never compares numerically with one peer and
   lexically with another — the exact shape of the cycle. NaN stays rejected from the numeric class
   (self-inconsistent), ±inf stay numeric.
4. **#2109**: reuse the already-shipped, already-unit-tested `smatchet::field_preview::FirstVisibleLine`
   in `RenderTextEditor` instead of the naive first-line cut.

## Files to modify

1. [Source/Core/src/Ui/SmatchetAboutUi.cpp](../../../Source/Core/src/Ui/SmatchetAboutUi.cpp) — #2093 popup-open guard + #2066 placeholder.
2. [Source/Core/src/SmatchetLocalization.cpp](../../../Source/Core/src/SmatchetLocalization.cpp) — #2066 `about.unknown` dictionary row.
3. [Source/Core/src/TicketGridSortPure.cpp](../../../Source/Core/src/TicketGridSortPure.cpp) — #2079 category-primary sort key.
4. [Source/Core/src/TicketFieldEditor.cpp](../../../Source/Core/src/TicketFieldEditor.cpp) — #2109 first-visible-line preview.
5. [tests/Core/AboutLocalization.test.cpp](../../../tests/Core/AboutLocalization.test.cpp) — #2066 SUBCASE.
6. [tests/Core/TicketGridSortPure.test.cpp](../../../tests/Core/TicketGridSortPure.test.cpp) — #2079 SWO sweep over a mixed numeric/non-numeric set.

## Existing utilities reused

- `smatchet::field_preview::FirstVisibleLine` — `Source/Core/include/Ui/FieldPreviewLinePure.h` — the
  blank-leading-line preview rule, already unit-tested and already used by `RenderClippedFieldText`.
- `SmatchetLocalization::TranslateSource` — `Source/Core/src/SmatchetLocalization.cpp:1558` — source-keyed
  chrome translation, the path every other About string already takes.
- `CompareCaseInsensitive` — `Source/Core/src/TicketGridSortPure.cpp:68` — total-order string tie-break.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction or split; four in-place fixes.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — `FirstVisibleLine` is the same single pass over the value the
  naive cut already did; the sort key adds at most one extra `strtod`/`strtoll` per comparison.
- **Pillar 2 (UI-thread never blocks)**: no impact — no I/O, no locking added.
- **Pillar 3 (never crash)**: the point of #2079 (SWO violation = `std::stable_sort` UB) and #2093
  (debug-build assert). Both are net crash-surface reductions.
- **Pillar 4 (accessibility)**: #2066 improves fr-FR completeness; no keyboard/contrast change.

## Perf-review-system gates

1. **PR-fast CI** — grid sort path → the active-project grid scenario already in
   `scripts/dev/perf-pr-fast-set.json`; no new scenario needed.
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms code path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **#2079 changes visible sort order** on mixed columns: numerics now group before non-numerics
  ascending, where before the grouping depended on comparison order (i.e. was undefined). Accepted —
  the old order was UB, and the new one is the documented Excel-style convention.
- **#2109 is diagnosed, not reproduced from a user session**: the Issue body is one line
  ("The body is full, but the field shows an empty line"). The editable-cell path is the only
  remaining surface that renders an empty line for a full value, and it matches the report exactly.
  Accepted; verification is the unit-tested helper plus the read-only path's precedent (#2097).
- **Non-goal**: #2099 (CI-native golden bootstrap) — explicitly out of scope this turn.
- **Non-goal**: any broader localization sweep of About DATA values — they stay verbatim by design.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: extended SWO sweep in `TicketGridSortPure.test.cpp`
  over a mixed set including `"5x"` (both directions, number-typed and untyped columns); new
  `AboutLocalization.test.cpp` SUBCASE pinning `TranslateSource("unknown") == u8"inconnu"` under fr-FR.
- **Bucket E (ImGui Test Engine)**: N/A — no new UI surface; the About change is a guard condition.
- **Bash-driver scenario / screenshot / sanitizer**: N/A.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in § Implementation log.

## Implementation log

_(filled post-ship)_

## Deviations

_(filled post-ship)_
