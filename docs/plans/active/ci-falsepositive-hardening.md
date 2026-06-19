# Plan — CI / gate false-positive hardening (Cluster C)

> **Slug**: `ci-falsepositive-hardening` (matches this file's basename without `.md`).
>
> **Status**: `active`

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
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
