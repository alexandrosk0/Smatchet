- 2026-08-06 · claude-code · [tooling] · P1 — bucket-C's sanctioned golden-diff step mask makes a *stale golden* indistinguishable from a *passing* one: a UI-pixel change ships, the capture stops matching, and nothing anywhere reports it

  Observed while making the `user-info-*` screenshot scenarios deterministic
  (PR #1962). Four `user-info-*` goldens deviate from a current capture at
  `y=[8,19] x=[273,296]`, `linf=81` — the menu-bar "Help" label, dim
  `(154,154,154)` in the golden vs bright `(232,232,232)` in the capture.
  Cause: PR #1937 (About dialog) moved `drawMenuBarHelpMenu(ctx)` outside the
  `trackerLocked` `BeginDisabled()` block
  ([`SmatchetUI_MainMenu.cpp:142-174`](../../../../Source/Core/src/Ui/SmatchetUI_MainMenu.cpp))
  and regenerated no goldens. Three more goldens
  (`code-syntax-coloring`, `command-palette-fuzzy`, `dock-gap-sentinel`,
  `linf≈240`) are stale from an older theme-palette change — window background
  `(15,15,15)` in the golden vs `(31,31,36)` in every capture; those files date
  to 2026-05-26/31. Seven stale goldens, none of which ever produced a signal.

  Mechanism. `AGENTS.md` § Merge gates lists bucket-C's golden diff as one of
  three sanctioned step-level masks — the step runs, the diff fails, the mask
  swallows the exit code, and the check reports green. The mask was justified by
  scenario nondeterminism (a flaky capture must not block merge), and that
  justification was real. But the mask is *total*: it discards the pass/fail
  signal entirely rather than downgrading it. A golden that is stale for a
  perfectly deterministic reason — a deliberate, permanent UI change the author
  simply did not regenerate for — is reported exactly like a clean run. The gate
  that exists to detect an unintended pixel change cannot report an intended one
  either, so goldens rot silently and the rot is only ever found by someone
  debugging something else.

  Compounding: nothing else keys on it. `postmortem-owed.sh` looks for
  merge-instant signals (non-SUCCESS checks, override labels, `Revert`, overdue
  deviations); a masked step emits none, so no owe is raised. The staleness has
  no expiry, no age nag, and no inventory — the only way to learn a golden is
  three months out of date is to diff it by hand.

  Proposed gate — **split reporting from blocking; never discard the verdict.**
  Two parts, either useful alone:

  1. **Report the diff even when the step is masked.** Have the bucket-C step
     always write its per-scenario verdicts (`name`, `linf`, band `y=[a,b]`,
     golden mtime) to a job-summary table / artifact regardless of the mask, and
     have the PR checks surface it. A stale golden then shows up on the PR that
     changed the pixels, attributable to that author, without gaining the power
     to block a flaky lane.
  2. **Graduate the now-deterministic subset to blocking.** PR #1962 removes the
     three nondeterminism sources behind the `user-info-*` flake (pre-`Draw`
     dispatcher-drain clobber, docked-tab focus, ephemeral update-modal focus
     theft) and measures 0/20 deviations twice. The flakiness that justified the
     mask no longer applies to that subset, so `user-info-*` can carry an
     unmasked diff while the remaining scenarios stay masked pending their own
     determinism work (the `ScenarioRunner::Tick` double-call is the known
     outstanding cause for scenarios that draw in `OnFrame`).

  Part 1 is the class fix and should land first — it converts *every* masked
  step from "silently discards its verdict" into "reports it," which is the
  property the other two sanctioned masks (fuzz-smoke's stochastic run, bucket-E's
  Mesa per-test run) lack for the same reason. Part 2 is the instance ratchet.

  **Status update 2026-08-15 — part 1 SHIPPED, part 2 still open.** The class fix
  landed: `scripts/dev/test-screenshot-diff.sh` writes a per-scenario verdict row
  (`scenario`, `verdict`, `linf`, `tol`, golden date, golden age in days) to its
  golden-report file on **every** outcome — pass, fail, bootstrap, missing
  capture, spawn failure — and the bucket-C job renders it into the job summary +
  uploads it as an artefact from a step that is `if: always()` and exits 0 on
  every branch, so reporting carries no blocking power. The two new steps sit
  **after** the lane-integrity step on purpose: a reporting step that could fail
  ahead of the teeth would skip them (lane-integrity carries no `if: always()`).
  `tests/bats/bucket_lane_launch_smoke.bats` pins the rows on the failing path,
  the git-sourced date on the passing path, the `-` fallback, and the opt-in (no
  env var → no file).

  **The date must come from git, not the filesystem** — caught by running the
  first build of this on the real lane (PR #2023, run 31904156001), where every
  row read `2026-08-15  0` while git dates those same goldens 2026-08-06/09. A
  CI checkout stamps every file with the checkout time, so an mtime-based age
  reports a months-old golden as brand new: the fresh-looking lie this report
  exists to kill, reintroduced by the report itself. The driver now dates each
  golden by the commit that last changed it and prints `-` when git cannot
  answer (untracked / shallow clone), never a number; the bucket-C checkout
  carries `fetch-depth: 0` so that history exists.

  **First-run finding (feeds part 2):** that same run reported all seven
  scenarios `fail` with `linf=-1` — the diff helper's *dimension mismatch*
  sentinel, not a pixel delta. Under llvmpipe on the CI runner the captures do
  not even match the goldens' dimensions, so part 2's ratchet cannot simply be
  flipped on for `user-info-*`: the CI-native capture size has to be reconciled
  first (or the ratchet scoped to a developer-GPU run). Lane status was `fail`,
  not `broken` (8 passed / 7 failed — the non-diff assertions pass), so
  lane-integrity stayed green and the mask swallowed all seven — exactly the
  silent rot described above, now visible on every run.

  The rule generalises in
  [`merge-gates.md`](../../../agent-rules/merge-gates.md) § Sanctioned step-level
  masks: *a mask may suppress blocking, never reporting* — the property the other
  two sanctioned masks (fuzz-smoke's stochastic run, bucket-E's Mesa per-test
  run) still lack, and the natural next application of this shape.
  Part 2 (graduate the now-deterministic `user-info-*` subset to an unmasked
  diff) is unchanged and still needs the golden regeneration below.

  Prerequisite for both: the stale goldens need regenerating, which is
  approval-gated by
  [`golden-image-approval.md`](../../../agent-rules/golden-image-approval.md) —
  an unmasked gate over a stale golden is a red check, not a signal. PR #1962
  cleared four of the seven with explicit approval (the `user-info-*` set), so
  part 2's ratchet is unblocked for that subset. The remaining three
  (`code-syntax-coloring`, `command-palette-fuzzy`, `dock-gap-sentinel`) are
  still stale and still masked — they need their own determinism work first,
  since the `ScenarioRunner::Tick` double-call
  ([`debt/2026-08-06-scenario-runner-ticks-twice-per-frame.md`](../debt/2026-08-06-scenario-runner-ticks-twice-per-frame.md))
  double-draws any scenario that renders from `OnFrame`.
  Status: partially-applied (part 1 — masked-step verdict reporting — shipped
    2026-08-15; part 2 — graduate the `user-info-*` subset to an unmasked diff —
    open, gated on regenerating the three remaining stale goldens)
  Last-reviewed: 2026-08-15
