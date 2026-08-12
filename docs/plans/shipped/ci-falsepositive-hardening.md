# Plan — CI / gate false-positive hardening (Cluster C)
<!-- plan-date: 2026-06-18 -->

> **Slug**: `ci-falsepositive-hardening` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

Cluster C of the 2026-06-18 self-improvement sweep — the "CI / gate false-RED & false-green" theme (11 backlog entries). These are gates that mis-fire: a check reds on noise/environment rather than real breakage, or under-reports (silently skips) and reads green when it shouldn't. This plan is a **campaign**; it ships in slices, one merge-able item at a time, and tracks the rest as a roadmap.

**Slice 1 (this PR) — unicode-bats silent-skip** (`test.md:10` `windows-bats-silently-skips-unicode-test-names` + its confirmed duplicate `tooling.md:476` `merge_gates.bats runs only 22/71 under a non-UTF-8 locale`). bats derives an internal function name from each `@test` description; under a non-UTF-8 ctype (the default on the Windows/MSYS dev box) bash can't parse a name with non-ASCII (`→`, `—`) and bats SILENTLY skips it as "unknown test name" — emitting no `ok`/`not ok` line, so the runner's `Passed/Failed` tally under-reports without failing (a false-green). Measured: `merge_gates.bats` ran only 43 of 123 tests locally (80 skipped).

Intended outcome: every `@test` runs at the pre-push gate regardless of dev-box locale; no silent skips.

## Approach

CI runs **no** bats (the windows-2022 runner lacks it — `.github/workflows/build-and-test.yml:311-314`); the bats suites run only at the pre-push aggregator `scripts/dev/test-all.sh`, which enrolls all 23 `test-*-bats.sh` runners by glob. So the DRY chokepoint is `test-all.sh`: detect an available UTF-8 locale (the name varies — `C.UTF-8` on Linux, `C.utf8` on MSYS; the entry's literal `C.UTF-8` is invalid on the Windows box) and `export LC_ALL`/`LANG` before running the runners. Every runner subprocess (and the `bats` it shells out to) inherits it → no skips. No `.bats` file is edited, so this is collision-free with any in-flight test PR and fixes all suites at once. Verified: `test-merge-gates.sh` went from 80 silent skips → 0, `Passed: 123 Failed: 0`.

The `C.UTF-8`-vs-`C.utf8` spelling is handled by matching `locale -a` with `grep -qix` (case-insensitive, whole-line) across a small candidate list, so the same block works on Linux and MSYS.

## Files to modify

1. `scripts/dev/test-all.sh` — detect + export a UTF-8 `LC_ALL`/`LANG` before the runner loop (Slice 1).
2. `docs/self-improvement/categories/{process,tooling}.md` — file two session learnings (see § Out-of-scope; filed alongside this slice).

## Existing utilities reused

- `scripts/dev/test-all.sh` runner-enrollment glob (line ~65) — the single chokepoint every bats runner passes through; the locale export rides in front of it.

## UX Pillar callouts

- **Pillars 1–4**: N/A — test-harness shell only; no product code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else N/A)

N/A — diff touches only `scripts/dev/test-all.sh` + docs.

## Risks / non-goals

- **Risk: un-skipping previously-hidden tests reveals real failures.** Checked — `merge_gates.bats` is `123/123` green under the locale (the skips hid coverage, not failures). If a future suite reveals a real red when un-skipped, that is the *true* state and should be fixed, not re-hidden.
- **Risk: no UTF-8 locale available at all.** The block is a no-op then (falls back to prior behaviour); not a regression. CI doesn't run bats so it's unaffected regardless.
- **Non-goal (this slice)**: per-runner "unknown test name → fail" detection for ad-hoc `bats <file>` runs outside `test-all.sh` (a defence-in-depth follow-up); and the remaining Cluster-C items below.

## Verification

- **Bash-driver**: `bash agents/scripts/core/test-merge-gates.sh` under the detected locale → 0 "unknown test name", `Passed: 123 Failed: 0` (was 43 run / 80 skipped). `shellcheck` clean on `test-all.sh`.
- **Build gate**: N/A — no C++.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test before finalising; record outcome.
- **Manual residue**: none — deterministic.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to deferred items before finalising.

Cluster-C roadmap (later slices, each its own PR):
- **perf-gate p99 false-positives** — `infra.md:229` (absolute p99 ceiling false-positives on job-correlated runner noise) + the open **P1** `tooling.md:187` (`p99-gate-warmup-frame-exclusion`); same p99 root, triage together.
- **comment-noise gate reds the required build + skips buckets** — `process.md:16` (lint runs inside the required Windows+MSVC job).
- **coverage-quarantine false-red** — `tooling.md:88`; **ci-infra-flake-reds-masquerade** — `infra.md:22`.
- **flakes** — `tooling.md:211` (comment_audit prose FP), `tooling.md:340` (shell-lint SIGPIPE), `test.md:62` (StubAiClientCancel wall-clock), `infra.md:99` (Bucket-E `--spawn`).

Session learnings filed with this PR (not Cluster-C items, but surfaced this session):
- `process.md` `pretooluse-git-guard-cwd-not-derivable-from-raw-command` (the unsound-`cd`-exemption lesson from PR #1388).
- `tooling.md` `merge-watcher-bats-repo-root-conflates-script-path-and-clone-path` (the worktree-only test-fail debt from PR #1393).

## Implementation log

- **Slice 1 — unicode-bats silent-skip.** Shipped: `scripts/dev/test-all.sh` detects + exports a UTF-8 `LC_ALL`/`LANG` before the bats runner loop (present on develop; the roadmap's original slice).
- **2026-07-14 — Cluster-C roadmap close-out (this PR).** The remaining roadmap items were either newly implemented or verified already-applied on develop:
  - **shell-lint SIGPIPE class → generalized (NEW).** Added **Rule 8** (`check_early_break_pipe`) to `agents/scripts/core/test-shell-lint.sh` — the #1593 follow-up. Beyond Rule 6's `$(… | head)` shape, it flags the general `producer | fn` case where `fn` is a same-script function that reads with `while … read` and `break`s early (early break → producer SIGPIPE 141 → script aborts under pipefail; CI-only, msys ignores SIGPIPE). Two-pass awk (fixed the `boundary(/re/)` function-arg-regex gotcha), conservative (requires while+read+break in the body). Fixtures `known-{bad,good}-8-early-break-pipe.sh` + two `shell_lint.bats` cases. **Verified locally**: fires on the bad fixture, silent on the good, **zero false-positives across all 293 real repo scripts**, dogfoods clean, full bats green.
  - **p99 relative-gate (NEW, greenlit).** Added the relative-p99 mechanism to `scripts/dev/perf-compare.py` (`p99_rel_pct` + `p99_min_abs_delta_ms` knobs, mirroring the relative-mean gate) + `regression-policy.json` default + `perf-baseline-schema.json`. Verified locally: silent when disabled, fires on real creep when armed, suppressed by the abs-Δ noise floor, markdown surfaces the armed knob. **ARMED 2026-07-14 on user sign-off** at `p99_rel_pct = 75.0` + `p99_min_abs_delta_ms = 1.5`, calibrated against the existing 108-sample CI distribution (`docs/perf/calibration-observations.md`): hottest-scope baseline p99 ~0.74-0.79 ms, worst observed 0.857 ms (positive swing ~0.065 ms, full range ~0.27 ms), so the 1.5 ms floor sits ~5.5× above the observed band and fires only on a genuine blowup to ≥~2.3 ms that stays under the 10 ms absolute ceiling. Re-verified against the observed distribution (silent on 0.857 / 0.90 / 1.6; fires on 2.5 / 6.0). Escape hatch: `perf-out-of-band` label.
  - **coverage-quarantine false-red — regression-pin (NEW).** The `coverage.sh` fix itself (the `--test-case-exclude=*[quarantined:*]*` on both captures + the test-binary-vs-tooling verdict split via `classify_capture_failure`) was already applied; the missing action-item was the fixture. Added a `coverage_gate.bats` case pinning that the quarantine-exclude flag is defined AND handed to both capture invocations, so a silent removal (the exact false-red regression) reds a discoverable test.
  - **comment-noise reds the required build + skips buckets — verified already shipped.** The high-integrity/comment-noise gate is already decoupled into its own required `comment-noise-gate` ubuntu lane (no `needs: windows-msvc`), so a style nit reds only its own check and no longer skips bucket-C/E (`.github/workflows/build-and-test.yml:223-277`). No change needed.
  - **StubAiClientCancel wall-clock — verified already shipped (#1280).** Both the outer wall-clock asserts and the stub's internal 100 ms cancel-ack budget are guarded with `SMATCHET_COVERAGE` (×8) in `StubAiClient.h` / `StubAiClientCancel.test.cpp`. No change needed.
  - **Bucket-E `--spawn` advisory-gate — verified already shipped (#1681).** No change needed.
  - **perf-gate p99 warmup-FP — superseded.** `infra.md` downgraded the absolute-ceiling FP to P3 ("nothing to gate; never an autonomous flip") after #1385 removed the warmup pollution; the relative-p99 mechanism above is the structural follow-up, shipped disabled.

## Deviations from plan

- **Slice-per-PR → single close-out PR.** The plan framed each roadmap item as its own PR; this session bundled the roadmap remainder (with `command-input-hardening` + `slice-g-db-corruption`) onto one branch at the user's direction. Each item is an independent, separately-revertable commit.
- **p99 gate: mechanism first, then armed on explicit sign-off.** The initial push added the mechanism + tests and left it disabled (the responsible default for a live merge gate). The user then explicitly signed off on arming it, so it was calibrated against the existing 108-sample CI distribution and armed (`p99_rel_pct = 75.0` + `p99_min_abs_delta_ms = 1.5`) in the same PR — mirroring how `mean_abs_ceiling_ms` shipped disabled then armed. The calibration (not the flip) is the load-bearing part: the abs-Δ floor sits ~5.5× above the observed runner p99 band.
- **Several roadmap items were already applied on develop** (comment-noise decouple, StubAiClientCancel, Bucket-E spawn, shell-lint SIGPIPE #1593, the coverage.sh exclude/split) — this PR verifies + regression-pins them rather than re-implementing.

## Verification (actual)

- **shell-lint Rule 8**: `bats tests/bats/shell_lint.bats` green (24 tests incl. the 2 new); full `test-shell-lint.sh` over 293 real scripts = `Passed: 293  Failed: 0` (no Rule-8 FP); dogfood self-lint clean.
- **p99 gate**: `perf-compare.py` imported + driven with synthetic baseline/fresh rows — disabled-default silent, armed-fires, noise-floor-suppressed, markdown-knob-shown all assert-pass; `regression-policy.json` + `perf-baseline-schema.json` valid JSON; `perf-compare.py` parses clean.
- **coverage-quarantine pin**: `bats tests/bats/coverage_gate.bats` green (5 tests incl. the new pin); `coverage.sh --selftest` PASS.
- **Lint gates on the whole diff**: `test-lint-rules.sh --diff origin/develop` PASS; `comment_audit.py --diff` clean; `shellcheck -S warning` clean on the modified linter.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
