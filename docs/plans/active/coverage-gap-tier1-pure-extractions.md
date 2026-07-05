# Plan — Coverage-gap Tier 1: pure extractions for the audit-fixed sort + long-text paths

> **Slug**: `coverage-gap-tier1-pure-extractions`
>
> **Status**: `active`

## Context

[`TEST_COVERAGE_GAP_MAP.md`](../../../TEST_COVERAGE_GAP_MAP.md) (merged #1594) ranks `TicketGridModel.cpp` and `TicketFieldEditor_Modal.cpp` as the top Tier-1 coverage gaps: both carry recently-remediated `CPP_CODE_AUDIT.md` defects (#3 duration-sort infinite loop, #1 long-text >64 KiB truncating write-back) whose fixes landed with **zero automated tests** — neither TU is compiled into any test target, so a regression would be invisible to the 65% aggregate floor and every other coverage gate. After this lands, the fixed logic is compiled into `SmatchetTests` behind pure seams and the two audit regressions are pinned by doctests.

## Approach

Apply the repo's established shell/core split: lift the byte-identical logic into dependency-light pure units and have the shells call them. (1) The duration-sort cluster (`SaturatingAccumulateDuration`, whole-string fast path, unit-by-unit parse, `CompareTimeTrackingValues`) moves from `TicketGridModel.cpp`'s anonymous namespace to a new `TicketGridDurationSortPure.{h,cpp}` — the existing `TicketGridModelKeySort.test.cpp` note documents why the grid-model TU itself can't link into tests (heavy transitive deps), which is exactly the seam this extraction cuts. (2) The modal's seed-truncation rule and Save-diff baseline become `PlanSeedCopy` / `ShouldQueueLongTextEdit` in the already-tested `TicketFieldEditorLongTextPure` unit; `SeedLongTextBuffer` / `CommitLongTextEdit` consume them. Also deletes `Source/Core/src/Test_JqlProjectScope.cpp`, the pre-harness compile-only relic the gap map flagged (currently swept into the product build by the `CORE_SOURCES` glob; superseded by `tests/Core/JqlProjectScope.test.cpp`).

## Files to modify

### Slice 1 (shipped — PR #1604, develop `3447bd0`)

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

### Slice 2 (this PR) — gap map Tier 1 rows #3–#4 (the "next slice" flagged below)

11. `Source/Core/include/Tracker/TrackerGridFieldDisplayPure.h` — NEW pure header (namespace `TrackerGridFieldDisplayPure`): the 6 render-model structs + `Build{Attachment,Watchers,Votes,Worklog,IssueRestriction,Progress}RenderModel`. Depends only on `Types/AttachmentTypes.h` (rank-0 leaf) — `AppController::AttachmentDescriptor` is spelled through its underlying `::AttachmentDescriptor` alias target so the pure TU never includes AppController.h.
12. `Source/Core/src/Tracker/TrackerGridFieldDisplayPure.cpp` — NEW TU; byte-identical lift of the model builders + their file-local `Parse*` helpers out of `TrackerGridFieldDisplay.cpp`'s anonymous namespace (deps: JsonParseUtil.h → ParseBounded, StringUtil.h, TrackerFieldValueParser.h `FormatWorkDurationFromSeconds`, nlohmann).
13. `Source/Core/src/Tracker/TrackerGridFieldDisplay.cpp` — shell: lifted block removed (1003 → ~500 lines); using-declarations pull the pure models/builders; draw code + per-value render caches + `Is*ColumnId` helpers stay.
14. `Source/Core/include/Tracker/TrackerDateTimePure.h` + `Source/Core/src/Tracker/TrackerDateTimePure.cpp` — add `InitDatePickerWorking` (picker seed, lifted byte-identical from the editor TU) and `PlanDateTimeCommit` + `DateTimeCommitPlan` (the Apply/Clear no-op-PUT commit gate with the canonical-form comparison; calls the already-tested `TicketFieldEditorCommitPolicyPure::ShouldCommitTouchPopupEdit`).
15. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp` — consumes both pure functions; the local `InitDatePickerWorking` and the inline Apply/Clear canonicalization block are removed.
16. `tests/Core/TrackerGridFieldDisplayPure.test.cpp` — NEW bucket-A suite: attachment fallback key-chains + explicit-empty vs unparsed, watcher/vote phrasing + negative clamps + loose-int coercions, worklog partial-page marker + 12-entry tooltip cap, issue-restriction shouldDisplay coercions, progress fast-path scanner + overflow clamp + DOM fallback, and a ParseBounded nesting-bomb regression across all six builders (the SECURITY_AUDIT.md unbounded-parse class this TU was flagged for).
17. `tests/Core/TrackerDateTimePure.test.cpp` — append seed-mode cases + the commit-gate cases (wire-format-only difference never queues; Clear-on-blank never queues; clamp-in-place; unparseable-current conservative).
18. `tests/CMakeLists.txt` — wire the display-pure test + production TU into `SmatchetTests` AND `SmatchetTsanTests` (the Linux-runnable subset — its TrackerFieldValueParser/Utils + CompactDateFormatPure closure already links there, which is what makes this slice locally verifiable in a Linux-only container).

### Slice 3 (this PR) — gap map Tier 1 row #5 (query-suggest engines)

19. `Source/Core/include/Tracker/JqlSuggestEnginePure.h` + `Source/Core/src/Tracker/JqlSuggestEnginePure.cpp` — NEW: the whole JQL autocomplete core (tokenizer, suggest-mode resolver, family-gated function catalog, non-system user filter, value/ORDER-BY appenders) lifted from `JqlSuggestEngine.cpp` and reparametrized `const AppController&` → `const std::vector<TrackerField>&` + `const std::vector<TrackerUser>&` (the cluster-A seam `TrackerQuerySuggestCommon` already uses; 5 call sites changed, logic otherwise byte-identical).
20. `Source/Core/src/Tracker/JqlSuggestEngine.cpp` — shell: 552 → 10 lines; the `AppController` overload forwards `GetAvailableFields()`/`GetAvailableUsers()` into the pure core.
21. `Source/Core/include/Tracker/PlaneQuerySuggestEnginePure.h` + `Source/Core/src/Tracker/PlaneQuerySuggestEnginePure.cpp` — NEW: same split for the Plane `fieldId:value` mini-language engine (fields vector only; 2 call sites changed).
22. `Source/Core/src/Tracker/PlaneQuerySuggestEngine.cpp` — shell: 202 → 10 lines.
23. `tests/Core/JqlSuggestEnginePure.test.cpp` — NEW bucket-A suite encoding the V21/V22 manual smokes from `backlog/MANUAL_TEST_QUEUE.md` (family-gated functions incl. the `membersOf("…")` caret sentinel, app/customer/inactive user suppression, email-prefix match) plus mode-resolution (operator/IS/logical/ORDER-BY chain), value quoting, JQL-escape of hostile display names, replace-span (selection + open-string), null-buffer/cursor-clamp, and the 80-item sort/cap.
24. `tests/Core/PlaneQuerySuggestEnginePure.test.cpp` — NEW bucket-A suite: `:`/`=` context parse with whitespace, value quoting, user-field live-search meta + "(display)" labels, logical fallback, null-buffer.
25. `tests/CMakeLists.txt` — wire both suites + pure TUs into `SmatchetTests` AND `SmatchetTsanTests` (+ `TrackerQuerySuggestCommon.cpp`/`JqlEscape.cpp` into the TSan subset, per the Slice-2 local-validation precedent).
26. `backlog/BACKLOG_CODE_REVIEW.md` — flip the stale N7/N11 rows the gap map flagged (tests rig + MarkdownConvert goldens exist since).

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

- Slice 1: shipped as PR #1604 (squash-merged to develop as `3447bd0`) — extraction + tests per § Files to modify items 1–10, plus two riders that PR picked up en route (the CI FetchContent self-heal and the cpp-httplib zstd auto-detect disable; see the PR body).
- Slice 2 (PR #1607, squash-merged to develop as `b9489af`): gap map Tier 1 rows #3–#4 per § Files to modify items 11–18. Both lifts verified byte-identical against the removed blocks; the display shell shrank 1003 → ~500 lines with zero behaviour change.
- Slice 3 (this PR, branch `claude/fable-5-codebase-improvements-l90taa`): gap map Tier 1 row #5 per § Files to modify items 19–26. Mechanical reparametrization scripted with asserted single-occurrence replacements; both suites executed locally (23 cases / 81 assertions green) before wiring.

## Deviations from plan

- Slice 1's § Archive step ("flip Status to shipped … in the same PR") did not happen in #1604 — the plan stayed `active` with its post-ship sections unpopulated. Rather than archiving a plan that had just gained a flagged "next slice", Slice 2 extends this same doc (the § Out of scope entry naming the `TrackerGridFieldDisplay` / `TrackerDateTimeFieldEditor` extractions is exactly this slice); the archive step executes when the map's Tier-1 extraction campaign leaves this doc with no flagged next slice.
- Slice 2's plan-lock seed could not be filed: the remote-session git proxy 403s pushes to `refs/locks/*` (branch pushes only). Per the seed contract (eager + non-blocking; ship-loops.md § Plan-lock seed exit-3 handling) the failure is logged here and the loop continues — Layer C's server-side gate still binds on the PR.
- Slice 2 registers its display-pure test in `SmatchetTsanTests` as well as `SmatchetTests` (the Slice-1 plan wired `SmatchetTests` only) — deliberate: the TSan subset is the only test target buildable in the Linux-container sessions doing this work, so local validation requires the test to live there too (it is thread-free; the TSan lane cost is one more pure TU compile).

## Verification (actual)

- `cmake --preset ninja-tsan-linux && cmake --build --preset ninja-tsan-linux && ctest` — the full Linux-runnable subset including both new/extended suites; results recorded in the PR body test plan.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` + `scripts/dev/test-docs.sh` — per the PR body test plan.
- Windows full rig (`ninja-test-msvc` ctest incl. the SmatchetTests registration) — CI.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Flip § Status to `shipped`, populate the three sections above, `git mv` to `docs/plans/shipped/` in the same PR.
