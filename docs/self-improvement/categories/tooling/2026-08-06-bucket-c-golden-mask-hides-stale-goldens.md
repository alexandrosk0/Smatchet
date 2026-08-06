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

  Prerequisite for both: the seven stale goldens need regenerating, which is
  approval-gated by
  [`golden-image-approval.md`](../../../agent-rules/golden-image-approval.md) —
  an unmasked gate over a stale golden is a red check, not a signal.
