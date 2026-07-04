# Plan — Coverage-gap Tier 1: pure extractions for the audit-fixed sort + long-text paths

> **Slug**: `coverage-gap-tier1-pure-extractions`
>
> **Status**: `active`

## Context

[`TEST_COVERAGE_GAP_MAP.md`](../../../TEST_COVERAGE_GAP_MAP.md) (merged #1594) ranks `TicketGridModel.cpp` and `TicketFieldEditor_Modal.cpp` as the top Tier-1 coverage gaps: both carry recently-remediated `CPP_CODE_AUDIT.md` defects (#3 duration-sort infinite loop, #1 long-text >64 KiB truncating write-back) whose fixes landed with **zero automated tests** — neither TU is compiled into any test target, so a regression would be invisible to the 65% aggregate floor and every other coverage gate. After this lands, the fixed logic is compiled into `SmatchetTests` behind pure seams and the two audit regressions are pinned by doctests.

## Approach

Apply the repo's established shell/core split: lift the byte-identical logic into dependency-light pure units and have the shells call them. (1) The duration-sort cluster (`SaturatingAccumulateDuration`, whole-string fast path, unit-by-unit parse, `CompareTimeTrackingValues`) moves from `TicketGridModel.cpp`'s anonymous namespace to a new `TicketGridDurationSortPure.{h,cpp}` — the existing `TicketGridModelKeySort.test.cpp` note documents why the grid-model TU itself can't link into tests (heavy transitive deps), which is exactly the seam this extraction cuts. (2) The modal's seed-truncation rule and Save-diff baseline become `PlanSeedCopy` / `ShouldQueueLongTextEdit` in the already-tested `TicketFieldEditorLongTextPure` unit; `SeedLongTextBuffer` / `CommitLongTextEdit` consume them. Also deletes `Source/Core/src/Test_JqlProjectScope.cpp`, the pre-harness compile-only relic the gap map flagged (currently swept into the product build by the `CORE_SOURCES` glob; superseded by `tests/Core/JqlProjectScope.test.cpp`).

## Files to modify

1. `Source/Core/include/TicketGridDurationSortPure.h` — NEW pure header (namespace `TicketGridDurationSortPure`; `rg -l 'TicketGridDurationSortPure|TicketGridSortPure' Source/Core/` confirmed no prior unit).
2. `Source/Core/src/TicketGridDurationSortPure.cpp` — NEW TU; byte-identical lift of the duration-sort cluster (deps: `<cctype>`, `<limits>` only). EXTRACT → this sink.
3. `Source/Core/src/TicketGridModel.cpp:19-134,319-326` — remove lifted block + local comparator; include the pure header; qualify the one call site. STAYS: field-id sets, render-plan logic.
4. `Source/Core/include/TicketFieldEditorLongTextPure.h` — add `LongTextSeedPlan`, `PlanSeedCopy`, `ShouldQueueLongTextEdit` declarations.
5. `Source/Core/src/TicketFieldEditorLongTextPure.cpp` — definitions (+ `<algorithm>`, `<cstddef>`).
6. `Source/Core/src/TicketFieldEditor_Modal.cpp:372-390,451-462` — `SeedLongTextBuffer` builds from `PlanSeedCopy`; `CommitLongTextEdit` gates on `ShouldQueueLongTextEdit`.
7. `Source/Core/src/Test_JqlProjectScope.cpp` — DELETE (pre-harness relic).
8. `tests/Core/TicketGridDurationSortPure.test.cpp` — NEW bucket-A suite: fast path, Jira pretty-print units, audit-#3 termination regressions ("2.5h", "(2h)", "3h 30m left"), audit-#19 saturation, comparator ordering.
9. `tests/Core/TicketFieldEditorLongTextPure.test.cpp` — append seed-plan + Save-diff regression cases (audit-#1: untouched truncated buffer never queues).
10. `tests/CMakeLists.txt` — wire the new test TU + production TU into `SmatchetTests`.

## Existing utilities reused

- `TicketFieldEditorLongTextPure` namespace + test file (`Source/Core/src/TicketFieldEditorLongTextPure.cpp`, `tests/Core/TicketFieldEditorLongTextPure.test.cpp`) — the seed/commit helpers join the unit that already owns the modal's pure logic instead of a new sibling.
- `SmatchetTests` direct-TU-link pattern (`tests/CMakeLists.txt:272` contract comment) — no new test infra.

## Extraction sizing

`TicketGridModel.cpp` 495 → ~370 lines (−~125 lifted, +1 include); new sink TU ~140 lines. `TicketFieldEditor_Modal.cpp` net ±0 (logic swapped for calls). No file approaches a cap; target cleared.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — byte-identical logic, one extra non-inlined call per compared cell pair during a sort; parsing dominates.
- **Pillar 2 (UI never freezes)**: strengthened — the always-terminates contract of the sort parser is now pinned by tests.
- **Pillar 3 (never crash)**: no impact — no new allocation/UB surface; saturation behaviour now tested.
- **Pillar 4 (accessibility)**: no impact (no UI change).

## Perf-review-system gates

1. **PR-fast CI**: N/A — behaviour-preserving extraction; no scenario's hot path changes shape.
2. **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*` (pure string functions only).
3. **Dispatcher drain**: untouched.
4. **Visible-cue bucket-E harness**: no new stall path.
5. **Marker inventory**: no new markers.

**Pre-push local check**: N/A — no perf-relevant change (see gate 1).

## Risks / non-goals

- **Risk**: extraction drift (lifted copy diverges from original). Mitigation: lift is byte-identical (verified by review of the removed block vs the new TU); tests encode current semantics including quirks (implicit `num=1` on bare non-unit chars).
- **Non-goal**: changing duration-parse semantics ("2.5h" still parses integer-wise — fixing fractional parsing is a product decision, not a coverage task).
- **Non-goal**: UTF-8-boundary-aware truncation in `PlanSeedCopy` (pre-existing behaviour; would change what users see).
- **Non-goal**: the rest of the gap map's tiers (tracked as follow-up slices).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `TicketGridDurationSortPure.test.cpp` (7 cases) + `TicketFieldEditorLongTextPure.test.cpp` seed-plan block (7 cases) green in `SmatchetTests`.
- **Bucket E (ImGui Test Engine)**: N/A — no UI behaviour change (existing long-text scenarios `LongTextOpenLargeAdfScenario` remain the E-side net).
- **Bash-driver scenario / screenshot / sanitizer**: existing sanitizer lanes cover the new TU via the test build; no new scenario.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: run pre-finalise; outcome recorded in § Deviations if it changes the plan.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: `TEST_COVERAGE_GAP_MAP.md` Tier 1 rows #3–#10 and Tiers 2–5 remain open; the map itself is the tracking doc (no stray refs to clear — grepped `CONTEXT*.md`, `docs/adr/`, `agents/*.md`).

- Per-file coverage ratchet (`coverage-perfile-gate.sh` `HIGH_RISK_UNITS`) additions for the two new units — follow-up once CI publishes their measured ≥90% rate; adding them unmeasured risks a red gate.
- `TrackerGridFieldDisplay` / `TrackerDateTimeFieldEditor` extractions — next slice.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Flip § Status to `shipped`, populate the three sections above, `git mv` to `docs/plans/shipped/` in the same PR.
