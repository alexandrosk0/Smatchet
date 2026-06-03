# Plan — Coverage threshold graduation (advisory → blocking)

> **Slug**: `coverage-threshold-graduation` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

The numeric line-coverage gate is **built and shipped but never switched on**, and its flip date has passed. Phase 9 of `docs/plans/shipped/test-suite-expansion-completion.md` landed the full apparatus — `scripts/dev/coverage.sh` (OpenCppCoverage, `--threshold N`), `.github/workflows/coverage.yml` (advisory: `--threshold 0`, `continue-on-error: true`), `coverage-gate.yml` + `coverage-delta-gate.sh` (the *structural* test-delta gate, already **hard-blocking** from day 1 with a `tests-out-of-band` escape). The numeric threshold was deliberately left at `0` for a 2-week soak. The workflow header and the shipped plan both name the flip: **`continue-on-error → false` + `--threshold 70` after two consecutive green weeks, target 2026-05-30.** Today is **2026-06-02 — the soak window has closed.**

One correctness gap blocks a fair flip. The end-state target is *"≥ 70% line coverage on `Source/Core/src/` **excluding ImGui/UI files**."* But `coverage.sh`'s `--excluded_sources` today carves out only third-party ImGui (`_deps`, `tests`, `Source.Plugins.Mcp.imgui`, `ImGui`, `imgui`) — **not Smatchet's own `Source/Core/src/Ui/`** draw code, which is hard to unit-test (it's bucket-E / screenshot-tested, not ctest-tested). Flipping to 70% against the un-excluded surface would red-bar `develop` on untestable UI lines, contradicting the spec.

**Intended outcome — one sentence:** after this lands, the numeric coverage gate measures the **testable** surface (`Source/Core/src/` minus `Ui/`) and **blocks** PRs below a data-chosen threshold (target 70%), completing the Phase 9 advisory→blocking lifecycle.

## Approach

Four slices, **data before flip**. The cardinal rule: never flip to a hardcoded `70` blind — measure the real number on the corrected surface first, then set the threshold from data (70 if we're there; a ramp floor + a raise-coverage backlog if we're not). A blocking gate that red-bars `develop` from minute one trains contributors to slap the override on everything.

1. **Measure-true.** Add `Source/Core/src/Ui/` + `Source/Core/include/Ui/` to `coverage.sh --excluded_sources` so the measured surface matches the end-state spec. Run `coverage.sh --threshold 0` once on a clean `develop` build and record the actual line %.
2. **Choose threshold from data.** If actual ≥ 70 → set `--threshold 70`. If actual < 70 → set the threshold to the current floor (rounded down to a stable integer) and file a `test-author` backlog to raise toward 70; the gate still graduates to *blocking at the floor* (prevents backslide) while the ramp continues. Either way the gate becomes real today; the *number* tracks reality.
3. **Confirm the precondition.** Verify **two consecutive green advisory weeks** of `coverage.yml` runs since Phase 9 (#148, ~2026-05-16). The 2026-05-30 calendar date is **necessary, not sufficient** — the locked decision is "two consecutive green weeks." If the advisory runs were flaky / no-op, extend the soak instead of flipping on the date alone.
4. **Flip.** `coverage.yml`: `continue-on-error: false` + `--threshold <chosen>`; rewrite the header comment from "advisory soak" to "blocking; threshold tracks `docs/high-integrity/`…". Add a `coverage-out-of-band` override label (parallel to `tests-out-of-band`) so a legitimate below-threshold PR has a low-friction escape distinct from the structural gate's.

The structural test-delta gate stays exactly as-is (already hard-blocking) — this plan only graduates the **numeric** sibling, the softer of the two.

## Files to modify

1. [`scripts/dev/coverage.sh`](scripts/dev/coverage.sh) (edit) — add `--excluded_sources "Source.Core.src.Ui"` + `"Source.Core.include.Ui"` to `OCC_FILTER_ARGS` (confirm OpenCppCoverage path-pattern match semantics at impl). Optional: a `--print-pct` flag echoing the measured number for the Slice-1 readout.
2. [`.github/workflows/coverage.yml`](.github/workflows/coverage.yml) (edit) — `continue-on-error: false`; `coverage.sh --xml-only --threshold <chosen>`; header comment rewrite (advisory → blocking, name the flip PR).
3. [`project.config.json`](project.config.json) (edit) — add `coverage-out-of-band` to `merge_gates.override_labels`; add a `coverage` block (`threshold: <chosen>`, `excluded: ["Source/Core/src/Ui", "Source/Core/include/Ui"]`) so the number is config-sourced, not hardcoded in two places.
4. [`docs/plans/shipped/test-suite-expansion-completion.md`](docs/plans/shipped/test-suite-expansion-completion.md) (edit, PR-only per § Plan revision) — one-line § Implementation-log / § End-state append: "line-coverage threshold flipped advisory→blocking at `<N>%` on `<date>` via this plan."
5. `docs/self-improvement/categories/test.md` (edit) — backlog the **≥90% high-risk-unit** target (IssueCreatePipeline, IssueDraft, TrackerFieldValueParser, CallstackParser, LocalCacheManager, TicketSyncService, ConfigManager migrations, MCP dispatch, Lua bindings) + (if Slice-2 floor < 70) the raise-to-70 ramp.

## Existing utilities reused

- `scripts/dev/coverage.sh` — the measurement wrapper; this plan only adds two exclusions + (optionally) a readout flag.
- `.github/workflows/coverage.yml` + `coverage-gate.yml` + `scripts/dev/coverage-delta-gate.sh` — the shipped gate pair; numeric flips, structural untouched.
- `merge_gates.override_labels` in `project.config.json` + the `tests-out-of-band` label-inspection pattern (`gh pr view --json labels`) — the `coverage-out-of-band` escape copies it.
- The shipped plan's § Locked decisions (Windows-runner-only, advisory→blocking lifecycle) — the precedent this plan completes.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — CI-config + a build-script exclusion. Indirectly strengthens Pillar 3 (never-crash) by enforcing test coverage on the strict-zone logic that guards crashes.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

`N/A — no `Source/Core/` source compiled-change. Edits `coverage.sh` (`scripts/dev/`, deny-listed) + `coverage.yml` (`.github/`, deny-listed) + `project.config.json` → not pure-docs (`is-pure-docs-diff.sh` false), but no `.cpp`/`.h`, so build/ctest/perf gates are no-ops. (If Slice 2 finds floor < 70 and tests are added to ramp, that test work is a separate PR with its own gates.)`

## Risks / non-goals

**Risks:**
- **Flip-blind red-bars `develop`** — the dominant risk. → Slice 1 measures the real % on the corrected surface *before* choosing the number; Slice 2 ramps from the floor if < 70. Never hardcode 70 without the readout.
- **CI-runner noise on a numeric gate** (the explicit reason Phase 9 soaked). → `coverage-out-of-band` escape + the structural gate already hard-blocks independently, so the numeric gate is the belt to the structural suspenders, not a solo chokepoint.
- **"Two green weeks" not actually met** despite the calendar passing (advisory runs flaky / headless no-op). → Slice 3 checks run history; extend soak if unproven. Calendar date ≠ green-week evidence.
- **OpenCppCoverage exclusion-pattern mismatch** (`Source.Core.src.Ui` vs the dotted/substring form OCC expects). → confirm against an actual `--excluded_sources` run in Slice 1; the readout will show whether `Ui/` lines dropped out.

**Non-goals:**
- **≥90% high-risk-unit threshold** — deferred (needs per-file Cobertura parsing; the 70% global floor is the MVP). Backlogged.
- Touching the **structural** test-delta gate — already hard-blocking; out of scope.
- **POSIX `lcov+gcov` path** — documented-not-wired in `coverage.sh`; stays Windows-runner-only per § Locked decisions.
- Raising coverage itself — if Slice 2 finds < 70, the *gate* graduates at the floor; the *ramp* (writing tests) is separate `test-author` work.

## Verification

- **Slice-1 readout**: `bash scripts/dev/coverage.sh --threshold 0 --print-pct` on a clean `build/ninja-test-msvc` prints the line % on the `Ui/`-excluded surface; confirm `Source/Core/src/Ui/*.cpp` lines are absent from `coverage/coverage.xml`.
- **Flip behavior**: a synthetic PR dropping a covered strict-zone unit below the chosen threshold **red-bars** `coverage.yml` (now `continue-on-error: false`); applying `coverage-out-of-band` downgrades it to pass; the structural `coverage-gate.yml` verdict is unchanged either way.
- **Shell lint**: `test-shell-lint.sh` on the edited `coverage.sh`.
- **Config integrity**: `project.config.json` still validates against `project.config.schema.json` (new `coverage` block + label).
- **Build gate**: `N/A — no compile in this PR.`
- **Manual residue**: the `coverage-out-of-band` label must be **created at repo level** (`gh label create coverage-out-of-band`) — same residue class the shipped plan flagged for `tests-out-of-band`; surfaced to the user, not silent.

## Out of scope (flagged, not designed)

- **≥90% high-risk-unit per-file gate** — follow-up plan after the global floor proves out.
- **POSIX coverage runner** — gated on a POSIX CI runner being provisioned.
- **Mutation testing** / branch-coverage floor — beyond the line-coverage end-state target.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
