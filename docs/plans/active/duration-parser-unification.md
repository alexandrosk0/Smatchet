<!-- index-summary: one duration parser behind grid sort + worklog edit — kill the "2.5h" divergence that sorts above 3h -->
# Plan — Duration-parser unification

> **Slug**: `duration-parser-unification` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Smatchet parses human duration strings (`"3h 30m"`, `"1w 2d"`) in **two independent implementations** that disagree on the same input, and the disagreement is user-visible in the grid.

| Input | `ParseWorkDurationToSeconds` (edit / submit) | `ParseDurationToSecondsForSort` (grid sort) | Arithmetically correct |
|---|---|---|---|
| `2.5h` | `0` | `18002` | `9000` |
| `1.5h` | `0` | `1 + 5×3600 = 18001` | `5400` |

The sort path (`Source/Core/src/TicketGridDurationSortPure.cpp:98-103`) treats **any** non-unit character as "add `num × 1` second, then advance". That rule exists for a good reason — it was the fix for an infinite loop where `pos` never moved and `num` was re-added forever (`CPP_CODE_AUDIT.md #3`, a permanent UI freeze on sort). But the chosen recovery silently manufactures a number: `2.5h` parses as `2` seconds (from the `.`) plus `5h`, giving 18002 s ≈ 5 h.

The consequence is a **wrong ordering in the live grid**: `2.5h` (18002) sorts *above* `3h` (10800), which sorts above `2h30m` (9000) — three renderings of overlapping durations in the wrong relative order. `CompareTimeTrackingValues` is wired into the real comparator at `Source/Core/src/TicketGridSortPure.cpp:140` for every field in `kTimeTrackingFieldIds`.

The edit path takes the opposite branch — `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:815` hits `else break`, so `1.5h` yields `0`. That one is *caught*: `HandleWorklogSave` rejects `sVal <= 0` with "Invalid Time spent format", and the field hint says `2w 4d 6h 45m`. So the edit path fails safe; only the sort path silently lies.

Both behaviours are **pinned as intended by tests** — `tests/Core/TicketGridDurationSortPure.test.cpp:40` asserts `== 2 + 5 * 3600`, `tests/Core/TrackerFieldValueParser.test.cpp:44` asserts `== 0` — with no comment acknowledging that the two disagree.

**Intended outcome**: after this lands, one parser backs both paths, a decimal duration either parses correctly or is rejected as unparseable, and no input produces a fabricated sort key that reorders real durations.

## Approach

Introduce a single pure parser with an **explicit tri-state outcome** — `Parsed(seconds)` / `Unparseable` / `Empty` — and put both call sites behind it. The current designs each collapse failure into an in-band number (`0` for edit, a char-count-derived integer for sort), and that collapse is the whole bug. An out-of-band failure signal lets each caller choose its own policy without inventing a value:

- **Sort** maps `Unparseable` to a stable sentinel ordered consistently (all unparseable values compare equal to each other and sort to one end), so garbage stays deterministic — the property the infinite-loop fix was protecting — without fabricating a magnitude.
- **Edit** maps `Unparseable` to the existing validation error, unchanged.

Decimal support is a **product decision the plan takes deliberately**: accept `2.5h` and compute `9000`. Jira's own time-tracking accepts decimal durations, so a tracker-returned `timeSpent` can legitimately contain one, and today the grid mis-sorts it. Rejecting decimals outright is the defensible alternative (it matches the current field hint) but leaves real Jira data unsortable, so acceptance wins. The field hint stays as-is — it documents the *recommended* form, not the only accepted one.

The infinite-loop guard is preserved structurally rather than by the add-one-second trick: the scan loop advances `pos` unconditionally on every iteration, and a token that fails to yield a unit sets `Unparseable` and stops rather than accumulating.

## Files to modify

1. `Source/Core/include/DurationParsePure.h` (NEW) — the tri-state result type + `ParseDurationSeconds(const std::string&)`. Header for a new pure TU; no deps beyond stdlib (must stay linkable into the focused test rig).
   > **Grep before naming**: confirm no existing `Duration*Pure` TU already owns this — `TicketGridDurationSortPure` and `TicketFieldEditorDurationPopupPure` both exist and neither is the right home (one is a comparator, one is popup focus logic).
2. `Source/Core/src/DurationParsePure.cpp` (NEW) — the implementation: trim, whole-integer fast path, then unit-by-unit scan with decimal support and unconditional `pos` advance. Carries the saturation logic (see reuse below).
3. `Source/Core/src/TicketGridDurationSortPure.cpp:121-141` (MOD) — `ParseDurationToSecondsForSort` becomes a thin adapter over the shared parser mapping `Unparseable` → sentinel; `CompareTimeTrackingValues` keeps its signature so `TicketGridSortPure.cpp:140` is untouched.
4. `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:750-827` (MOD) — `ParseWorkDurationToSeconds` becomes a thin adapter mapping `Unparseable`/`Empty` → `0`, preserving its current contract for every caller.
5. `tests/Core/DurationParsePure.test.cpp` (NEW) — the behavioural spec: decimals, multi-unit, saturation, unparseable classification, and the **cross-path agreement property** (for every fixture, sort and edit derive from the same seconds).
6. `tests/Core/TicketGridDurationSortPure.test.cpp:40,43-47` (MOD) — re-pin the changed expectations. `"2.5h"` becomes `9000`. The garbage cases (`"a b c" == 3`, `"..." == 3`, `"~" == 1`, `"(2h)" == 1 + 2*3600 + 1`) change to the sentinel; **each re-pinned line gets a comment naming why**, so the next reader does not read the new numbers as arbitrary.
7. `tests/Core/TrackerFieldValueParser.test.cpp:44` (MOD) — `"1.5h"` moves from `0` to `5400`. Note this *widens* what the worklog dialog accepts; that is the intended product change.
8. `tests/fuzz/fuzz_duration_parse.cpp:18` (MOD) — retarget at the shared parser so the fuzz lane covers the new decimal + sentinel paths.
9. `tests/CMakeLists.txt` (MOD) — register `DurationParsePure.test.cpp` + `DurationParsePure.cpp`.
10. `CMakeLists.txt` (MOD) — add `DurationParsePure.cpp` to the Core source list for **both** `SmatchetStandalone` and `SmatchetCore_DX12`.

## Existing utilities reused

- `SaturatingAccumulateDuration` — `Source/Core/src/TicketGridDurationSortPure.cpp:16` — the overflow-safe accumulator (`CPP_CODE_AUDIT.md #19`); **move** it into the shared TU rather than re-rolling. It already handles the hostile-magnitude case the fuzz lane exercises.
- `kMaxDurationSeconds` — `TicketGridDurationSortPure.cpp:14` — the saturation ceiling; moves with the accumulator.
- `kMaxDurationUnits` / `kMaxTotalSeconds` — `TrackerFieldValueParser.cpp:799,821` — the *edit* path's independently-chosen ceilings. Reconcile to one pair in the shared TU; note they currently differ (`1e9` units / `1e15` s vs `LLONG_MAX/2`), so picking one is a real decision, not a merge.
- `TrimCopyAsciiWhitespace` — `Source/Core/include/StringUtil.h:9` — replaces the local `TrimSpacesTabs` (`TicketGridDurationSortPure.cpp:29`). Note the two differ: the local one trims only space/tab, the shared one also trims `\n`/`\r`. Confirm the widening is harmless for tracker-supplied values before substituting.
- `FormatWorkDurationFromSeconds` — `Source/Core/src/Tracker/TrackerFieldValueParser.cpp` — the inverse; the existing round-trip test (`TrackerFieldValueParser.test.cpp:76`) must stay green and is the best regression signal.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

Extracts the parse core out of two TUs into one shared pure TU.

- **EXTRACT → `DurationParsePure.cpp`**: the scan loop, unit table, saturation accumulator, trim (~90 lines net, deduplicated from ~140 across the two sources).
- **STAYS in `TicketGridDurationSortPure.cpp`**: `CompareTimeTrackingValues` + the sentinel-ordering policy (~25 lines).
- **STAYS in `TrackerFieldValueParser.cpp`**: the `0`-on-failure adapter (~8 lines) and every unrelated field-parsing function.

Both sources net-shrink; neither is near a cap today, so this is a correctness-driven extraction rather than a size-driven one.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the parser runs inside a **sort comparator** — O(n log n) invocations per sort of a duration column. The shared implementation must stay allocation-light: no `std::string` copy per call in the hot path (the current `TrimSpacesTabs` returns a copy and `ParseDurationUnitsSum` calls `s.substr(pos)` **inside the loop**, allocating per token — a pre-existing wart worth fixing while here). Measure before/after on a duration-column sort.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact — pure CPU, no I/O; the grid sort already runs on the UI thread and stays bounded.
- **Pillar 3 (never crash)**: net positive. The unconditional-advance invariant is preserved *structurally* (loop advances every iteration) rather than as a side effect of the add-one-second rule, so the `CPP_CODE_AUDIT.md #3` freeze cannot regress. Saturation is retained, so hostile magnitudes still cannot signed-overflow (UB). The fuzz lane is retargeted at the new entry point.
- **Pillar 4 (accessibility)**: no impact.

## Perf-review-system gates

Diff touches `Source/Core/` — declaring each:

1. **PR-fast CI** — a grid-sort scenario is the closest mapped path; check `scripts/dev/perf-pr-fast-set.json` for a sort scenario over a duration column and add one if absent. This is the plan's one genuine perf-sensitive surface (comparator hot path).
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new sync stall > 100 ms.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline against the sort scenario before opening the PR.

## Risks / non-goals

- **Risk — re-pinning tests looks like weakening them.** Changing an assertion to match new behaviour is exactly what a reviewer should be suspicious of. *Mitigation*: every changed assertion gets an inline comment stating the old value, the new value, and why; the PR body carries the before/after table from § Context.
- **Risk — widening accepted worklog input changes what gets submitted.** `1.5h` currently errors; after this it submits 5400 s. That is the intent, but it is a **behaviour change on a mutation path**, not a pure refactor. *Mitigation*: call it out explicitly in the PR body; the round-trip test through `FormatWorkDurationFromSeconds` pins that the submitted value is what the user meant.
- **Risk — the sentinel changes sort order for garbage rows.** Rows with unparseable duration text will move relative to today. *Mitigation*: accepted and intended — today's ordering of garbage is a character-count artefact, not meaningful. Pick "unparseable sorts last in ascending" and pin it.
- **Risk — reconciling the two saturation ceilings changes clamping.** The paths currently clamp differently. *Mitigation*: pick the *lower* ceiling pair, verify the fuzz corpus still saturates rather than overflowing, and note the choice in the impl log.
- **Risk — per-call allocation regresses the comparator.** *Mitigation*: named as a Pillar 1 callout with a measurement obligation, not left implicit.
- **Non-goal — a full duration grammar** (`"1h30"`, `"90 minutes"`, localised unit words). Out of scope; the unit set stays `w/d/h/m` plus bare seconds.
- **Non-goal — changing `FormatWorkDurationFromSeconds`.** The formatter is correct; only parsing is in scope.
- **Non-goal — changing the field hint text.** `2w 4d 6h 45m` stays as the recommended form.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `tests/Core/DurationParsePure.test.cpp` is the primary gate — decimals (`2.5h == 9000`, `0.5h == 1800`), multi-unit, mixed decimal + multi-unit, bare seconds, saturation at both ceilings, unparseable classification (`"a b c"`, `"~"`, `"..."`, `"3h 30m left"`), empty/whitespace, and a **property test** asserting sort and edit agree on every fixture. Existing `TicketGridDurationSortPure` / `TrackerFieldValueParser` suites stay green modulo the re-pinned lines.
- **Bucket E (ImGui Test Engine)**: N/A — no new UI surface. The grid sort is already covered by existing bucket-E grid scenarios; a red there would catch a comparator contract break.
- **Fuzz**: `tests/fuzz/fuzz_duration_parse.cpp` retargeted; the `fuzz-smoke.yml` lane must stay green (it is the guard against a re-introduced infinite loop).
- **Bash-driver scenario / screenshot / sanitizer**: nightly ASan/UBSan covers the new TU — UBSan specifically guards the signed-overflow path the saturation logic exists to prevent.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) — **anchored to steps 9-10**, since this plan adds a TU to the Core source list and a missed target is a link error only DX12 would show.
- **Perf**: PR-fast sort scenario within budget vs baseline; record the before/after in § Verification (actual).
- **Doc validation (blocks plan-doc PRs)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: **not yet run** — owed before implementation starts. Sharpen "unparseable" vs "empty" vs "zero" in particular; those three are conflated in today's code and the whole plan turns on separating them.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here.

- **`TicketFieldEditorDurationPopupPure`** — a third duration-adjacent unit, but it handles popup focus/commit decisions, not parsing. Confirmed not a third parser; no action.
- **Localised duration units** — the app localises UI text (en-US/fr-FR) but duration units stay ASCII `w/d/h/m`. Not designed; would need a `SmatchetLocalization` seam in a pure TU, which the purity constraint forbids.
- **Duration *display* consistency across backends** — Jira/Plane/GitHub/Linear may each render durations differently upstream; this plan unifies parsing only.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
