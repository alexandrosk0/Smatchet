# Agent self-improvement — applied (archive partition 2026-07)

> Rotated slice of [`applied.md`](applied.md) (see its header for format /
> categories / workflow). Entries whose original surface date falls in
> 2026-07, sorted latest first. Append-only via
> `agents/scripts/core/rotate-applied-md.sh`; do not file new work here.
>
> **Deleted-runtime banner (2026-05-21)** — entries below that reference the
> agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`,
> `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`,
> `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`,
> `CiFailureClassifier`, the `dispatch_source` enum, the sentinel-file protocol,
> `agent/<proposalId>` worktrees, the `coderabbit-react-loop` design,
> `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code
> removed 2026-05-21 (v1 PR1 of `../../plans/shipped/github-tracker-backend.md`,
> merge sha `b1d241bc`). Preserved as historical record of what was tried.

<!-- Latest first. Appended by rotate-applied-md.sh only. -->

- 2026-07-13 · orchestrator (mutation-smoke Phase 3 corpus expansion) · [test] · P2 — the Phase-3 mutation sweep (`docs/plans/mutation-smoke-gate.md`, 23 new mutants over the 13 TUs the pilot didn't reach) found **1 new genuine weak assertion**: `JiraErrorMessagePure.test.cpp` "cap never splits a multi-byte UTF-8 sequence" never executed the truncation backoff it documents
  Details: the test built a 200×'é' = exactly-400-byte message; `AppendCapped`'s `out.size() + candidate.size() <= kMaxJoinedErrorLen` (400) appended it whole, so the UTF-8 lead-byte backoff loop never ran and mutant JIRAERR-02 (`== 0x80u` → `!= 0x80u` in the continuation-byte test) survived. Same at-the-boundary-but-not-past-it shape as the pilot's MergeWatch-m3 finding — a test that stops exactly at a cap asserts nothing about the over-cap branch.
  Resolution: applied — test now uses 300×'é' (600 B, past the cap) and additionally asserts the trailing ellipsis marker (proof the truncation path engaged, so the case can never silently regress to a no-op again); JIRAERR-02 re-run SURVIVED → KILLED, suite 2150/2150 green. Guard kept in `scripts/dev/mutation-smoke-corpus.json` as a permanent regression guard.
  Status: applied
  Last-reviewed: 2026-07-13

<!-- reconcile round 2 (2026-07-11): entries below were fixed on develop but never marked applied; verified against the tree and archived. -->

# `daemon_loop` bats tests don't stub `maybe_self_resync`, so they run real git/network and flake in the required selftests lane

- 2026-07-13 · orchestrator (mutation-smoke Phase 3 corpus expansion) · [test] · P2 — the Phase-3 mutation sweep (`docs/plans/mutation-smoke-gate.md`, 23 new mutants over the 13 TUs the pilot didn't reach) found **1 new genuine weak assertion**: `JiraErrorMessagePure.test.cpp` "cap never splits a multi-byte UTF-8 sequence" never executed the truncation backoff it documents
  Details: the test built a 200×'é' = exactly-400-byte message; `AppendCapped`'s `out.size() + candidate.size() <= kMaxJoinedErrorLen` (400) appended it whole, so the UTF-8 lead-byte backoff loop never ran and mutant JIRAERR-02 (`== 0x80u` → `!= 0x80u` in the continuation-byte test) survived. Same at-the-boundary-but-not-past-it shape as the pilot's MergeWatch-m3 finding — a test that stops exactly at a cap asserts nothing about the over-cap branch.
  Resolution: applied — test now uses 300×'é' (600 B, past the cap) and additionally asserts the trailing ellipsis marker (proof the truncation path engaged, so the case can never silently regress to a no-op again); JIRAERR-02 re-run SURVIVED → KILLED, suite 2150/2150 green. Guard kept in `scripts/dev/mutation-smoke-corpus.json` as a permanent regression guard.
  Status: applied
  Last-reviewed: 2026-07-13

<!-- reconcile round 2 (2026-07-11): entries below were fixed on develop but never marked applied; verified against the tree and archived. -->

# `daemon_loop` bats tests don't stub `maybe_self_resync`, so they run real git/network and flake in the required selftests lane

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [test] · P1 — `test-merge-watcher-bats.sh` test 30 fails ~1-in-5 runs because `daemon_loop` calls `maybe_self_resync(0)` at startup and the test never stubs it, so a "unit" test exercises real `git fetch` + drift detection

## Friction

`tests/bats/merge_watcher.bats:679` ("daemon_loop per-PR backstop: a transient
exception in one PR is logged + the loop continues") drives `mw.daemon_loop(0)`
with `process_registered_pr`, `read_registry`, `write_pid_file`,
`clear_pid_file`, and `time.sleep` all monkeypatched — but **not**
`maybe_self_resync`. `daemon_loop` (verified `agents/scripts/core/merge-watcher.py:3171`)
unconditionally calls `maybe_self_resync(0)` *before* the poll loop as a startup
gate-freshness check, and that function runs a real bounded `git fetch` + drift
detection against the live checkout (and "may re-exec on POSIX"). So the test's
outcome depends on network latency and the working tree's drift state at run
time — it passed 5/6 local runs and failed the 6th on exactly this test, and it
reddened the required `Agentic self-tests (bats)` lane on an unrelated docs-only
PR (#1718). The sibling `daemon_loop` tests at :709 and :739 have the same latent
gap.

The failure surfaces as a wrong `seen:`/missing-WARN assertion, which reads like a
logic regression but is pure test-isolation leakage — a false red that costs a
diagnosis round and blocks merge on a flake.

## Proposal

Stub `mw.maybe_self_resync = lambda *_a, **_k: {}` (a no-op returning an empty
dict, matching its contract of `.get('resync_action')` / `.get('resync_needs_*')`)
in the four `daemon_loop` tests (:679, :709, :739, :771) alongside the existing
`write_pid_file`/`time.sleep` stubs, so `daemon_loop` never touches git/network in
a unit test. Optionally add a module-level guard so `daemon_loop`'s startup resync
is skippable via an env knob the tests already set. Est ~15 min. Deterministic
after — the assertions are otherwise fully specified by the faked registry.

**Update (2026-07-10): fixed in this PR (#1718).** Added the
`mw.maybe_self_resync` no-op stub to all four `daemon_loop` tests; the suite went
8/8 green locally (was ~1-in-5 red on test 30). Archive to `applied.md` on the
next self-improvement sweep.

## Format

- Details: see § Friction. Verified: `merge-watcher.py:3171` `daemon_loop` calls
  `maybe_self_resync(0)` unconditionally; the test at `merge_watcher.bats:679`
  stubs five symbols but not `maybe_self_resync`; observed 1/6 local failure on
  test 30 and the CI red on #1718 head `9101c9c7`.
- Concrete next action: see § Proposal.
- Status: applied (stale `Status: open` reconciled during the 2026-08-12 archival sweep — entry already lived in applied.md)
- Last-reviewed: 2026-07-10

  Status: applied (2026-07-11 reconcile — verified fixed on develop: all four `daemon_loop` tests in tests/bats/merge_watcher.bats (:679/:710/:741/:774) now stub `mw.maybe_self_resync = lambda *_a, **_k: {}`, so the startup resync never touches real git/network.)

---

# Perf gate is required but its mean-budget teeth are still unarmed (step-5 calibration owed)

- **Category:** test
- **Priority:** P2
- **Date:** 2026-07-05
- **Status:** RESOLVED 2026-07-06 — mean budget armed (`mean_abs_ceiling_ms = 6.94`); plan shipped: [`docs/plans/shipped/perf-gate-step5-calibration.md`](../../plans/shipped/perf-gate-step5-calibration.md)

## What I hit

Auditing "is the perf gate mandatory / healthy" after the all-gates-blocking flip, I confirmed `Perf PR-fast (windows-2022)` **is** a required branch-protection context **and** blocks via the poller's `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` — so a perf red genuinely blocks merge. Good. But two teeth are still retracted, and neither is obvious from the green checkmark:

1. **Mean budget disabled.** `regression-policy.json → default.mean_abs_ceiling_ms = null`. The Pillar-1 steady-state budget (6.94 ms / 144 Hz) is **not** enforced — a scope could sit at 8 ms `avgPerCallMs` and pass. This is a *documented, deliberate* deferral ("perf-gate-revival step-5 calibration"), not a bug — but it has sat null since 2026-06-07 with no follow-up plan, so it reads as done when it isn't.

2. **Relative-regression coverage is thin because baselines are shallow.** Every committed `ci-windows-latest` baseline has per-scope `calls = 1–2` (only ONE scope across all six scenarios clears `min_baseline_calls = 10`). The relative 10%-delta gate skips every below-floor row *by design* (single-frame % swings are noise) — correct, but it means the relative gate is effectively a no-op for ~99% of scopes today. The absolute p99 (≤10 ms) + max (≤50 ms) ceilings *do* fire on every row (CR-949-1), so the gate isn't toothless — but steady-state drift below those ceilings is uncaught.

Secondary: the committed baselines predate the `p99Ms` emitter (`GetLastFrameRows(includeP99=true)` shipped after capture), so baseline rows carry no `p99Ms` — the p99 ceiling works off the *fresh* run's absolute value only, and every p99 baseline-delta reads "(new)".

## Why it matters

"Gate, don't trust": a green `Perf PR-fast` currently certifies *no p99/max blowup*, not *within the 6.94 ms steady-state budget*. That gap is invisible to anyone reading the check status, and the calibration that closes it has no owning plan.

## Fix

Tracked in the plan doc (arm `mean_abs_ceiling_ms` + per-scenario overrides from observed CI runs; recapture baselines so p99 + call depth are real; decide whether to deepen scenario frame counts). Tightening a live gate's numbers is a human-judgment call — the plan gates it behind observed-run evidence + user sign-off, never an autonomous flip.

## Self-improvement

Empty.

  Status: applied (2026-07-11 reconcile — verified fixed on develop: docs/perf/regression-policy.json `mean_abs_ceiling_ms` is ARMED at 6.94 (perf-gate step-5 calibration pass, 2026-07-06) with an empty perScenario map; baselines recaptured #1659.)

---

# `test-orphan-bats` runs only in the full suite / CI, not the pre-ship fast path — an unwrapped `.bats` reddens develop a merge later

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [tooling] · P2 — a new bats suite shipped without its `test-*.sh` wrapper; the orphan-bats gate caught it only in CI, on the *next* PR, masking which change introduced the red

## Friction

The mutation-smoke gate slice (#1698) added `tests/bats/mutation_smoke.bats`
without a `test-*.sh` wrapper naming its path. `test-orphan-bats.sh` (which
enforces that every bats suite has a runner, so an added suite can't silently
never-run) **is** auto-enrolled in `scripts/dev/test-all.sh` — verified:
`test-all.sh:113` globs `agents/scripts/core/test-*.sh` and orphan-bats lives
there — but `scripts/dev/pre-ship.sh`, the fast pre-push gate, does **not** run
it (verified: `grep -c orphan scripts/dev/pre-ship.sh` → 0).

So the orphan escaped the local pre-push loop and surfaced only as a red
`Agentic self-tests (bats)` lane on the **next** PR's CI (#1702), one merge after
the change that caused it — the red pointed at an innocent PR and cost a
diagnosis round to trace back to #1698. A trap: the mirror script
`scripts/dev/test-mutation-smoke.sh` *looks* like it covers the suite but it
validates the harness/corpus, not the bats file, so it does not satisfy the
wrapper requirement.

## Proposal

1. Add a fast `bash agents/scripts/core/test-orphan-bats.sh` call to
   `scripts/dev/pre-ship.sh` (the check is near-instant — no build, just a glob +
   grep over wrappers) so an unwrapped suite is caught before push, not a merge
   later on an unrelated PR's CI.
2. Playbook one-liner: **adding a `tests/bats/*.bats` requires its
   `test-<name>-bats.sh` wrapper (naming the suite by `tests/bats/<name>.bats`
   path) in the SAME PR** — a harness/corpus mirror script does not count.

Est ~15 min total. This session fixed the instance by adding
`scripts/dev/test-mutation-smoke-bats.sh` (#1702), but the pre-ship gap remains
and will bite the next suite added without a wrapper.

**Update (2026-07-10): implemented.** Added a
`bash agents/scripts/core/test-orphan-bats.sh` stage to `scripts/dev/pre-ship.sh`
(next to the test-list consistency check), so a wrapper-less bats suite is caught
before push. Archive to `applied.md` on the next sweep.

## Format

- Details: see § Friction. Verified against the committed tree at develop head.
- Concrete next action: see § Proposal (1)–(2) — done.
- Status: applied (stale `Status: open` reconciled during the 2026-08-12 archival sweep — entry already lived in applied.md)
- Last-reviewed: 2026-07-10

  Status: applied (2026-07-11 reconcile — verified fixed on develop: scripts/dev/pre-ship.sh:316 runs `bash agents/scripts/core/test-orphan-bats.sh` in the fast pre-push path.)


<!-- reconcile 2026-07-11: entries below were `Status: applied` in categories/<cat>/ but never moved here; archived in one batch (PR reconcile). -->

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [test] · P1 — `test-merge-watcher-bats.sh` test 30 fails ~1-in-5 runs because `daemon_loop` calls `maybe_self_resync(0)` at startup and the test never stubs it, so a "unit" test exercises real `git fetch` + drift detection

## Friction

`tests/bats/merge_watcher.bats:679` ("daemon_loop per-PR backstop: a transient
exception in one PR is logged + the loop continues") drives `mw.daemon_loop(0)`
with `process_registered_pr`, `read_registry`, `write_pid_file`,
`clear_pid_file`, and `time.sleep` all monkeypatched — but **not**
`maybe_self_resync`. `daemon_loop` (verified `agents/scripts/core/merge-watcher.py:3171`)
unconditionally calls `maybe_self_resync(0)` *before* the poll loop as a startup
gate-freshness check, and that function runs a real bounded `git fetch` + drift
detection against the live checkout (and "may re-exec on POSIX"). So the test's
outcome depends on network latency and the working tree's drift state at run
time — it passed 5/6 local runs and failed the 6th on exactly this test, and it
reddened the required `Agentic self-tests (bats)` lane on an unrelated docs-only
PR (#1718). The sibling `daemon_loop` tests at :709 and :739 have the same latent
gap.

The failure surfaces as a wrong `seen:`/missing-WARN assertion, which reads like a
logic regression but is pure test-isolation leakage — a false red that costs a
diagnosis round and blocks merge on a flake.

## Proposal

Stub `mw.maybe_self_resync = lambda *_a, **_k: {}` (a no-op returning an empty
dict, matching its contract of `.get('resync_action')` / `.get('resync_needs_*')`)
in the four `daemon_loop` tests (:679, :709, :739, :771) alongside the existing
`write_pid_file`/`time.sleep` stubs, so `daemon_loop` never touches git/network in
a unit test. Optionally add a module-level guard so `daemon_loop`'s startup resync
is skippable via an env knob the tests already set. Est ~15 min. Deterministic
after — the assertions are otherwise fully specified by the faked registry.

**Update (2026-07-10): fixed in this PR (#1718).** Added the
`mw.maybe_self_resync` no-op stub to all four `daemon_loop` tests; the suite went
8/8 green locally (was ~1-in-5 red on test 30). Archive to `applied.md` on the
next self-improvement sweep.

## Format

- Details: see § Friction. Verified: `merge-watcher.py:3171` `daemon_loop` calls
  `maybe_self_resync(0)` unconditionally; the test at `merge_watcher.bats:679`
  stubs five symbols but not `maybe_self_resync`; observed 1/6 local failure on
  test 30 and the CI red on #1718 head `9101c9c7`.
- Concrete next action: see § Proposal.
- Status: applied (stale `Status: open` reconciled during the 2026-08-12 archival sweep — entry already lived in applied.md)
- Last-reviewed: 2026-07-10

  Status: applied (2026-07-11 reconcile — verified fixed on develop: all four `daemon_loop` tests in tests/bats/merge_watcher.bats (:679/:710/:741/:774) now stub `mw.maybe_self_resync = lambda *_a, **_k: {}`, so the startup resync never touches real git/network.)

---

# Perf gate is required but its mean-budget teeth are still unarmed (step-5 calibration owed)

- **Category:** test
- **Priority:** P2
- **Date:** 2026-07-05
- **Status:** RESOLVED 2026-07-06 — mean budget armed (`mean_abs_ceiling_ms = 6.94`); plan shipped: [`docs/plans/shipped/perf-gate-step5-calibration.md`](../../plans/shipped/perf-gate-step5-calibration.md)

## What I hit

Auditing "is the perf gate mandatory / healthy" after the all-gates-blocking flip, I confirmed `Perf PR-fast (windows-2022)` **is** a required branch-protection context **and** blocks via the poller's `MERGE_GATES_BLOCK_ALLOWLIST_RE="."` — so a perf red genuinely blocks merge. Good. But two teeth are still retracted, and neither is obvious from the green checkmark:

1. **Mean budget disabled.** `regression-policy.json → default.mean_abs_ceiling_ms = null`. The Pillar-1 steady-state budget (6.94 ms / 144 Hz) is **not** enforced — a scope could sit at 8 ms `avgPerCallMs` and pass. This is a *documented, deliberate* deferral ("perf-gate-revival step-5 calibration"), not a bug — but it has sat null since 2026-06-07 with no follow-up plan, so it reads as done when it isn't.

2. **Relative-regression coverage is thin because baselines are shallow.** Every committed `ci-windows-latest` baseline has per-scope `calls = 1–2` (only ONE scope across all six scenarios clears `min_baseline_calls = 10`). The relative 10%-delta gate skips every below-floor row *by design* (single-frame % swings are noise) — correct, but it means the relative gate is effectively a no-op for ~99% of scopes today. The absolute p99 (≤10 ms) + max (≤50 ms) ceilings *do* fire on every row (CR-949-1), so the gate isn't toothless — but steady-state drift below those ceilings is uncaught.

Secondary: the committed baselines predate the `p99Ms` emitter (`GetLastFrameRows(includeP99=true)` shipped after capture), so baseline rows carry no `p99Ms` — the p99 ceiling works off the *fresh* run's absolute value only, and every p99 baseline-delta reads "(new)".

## Why it matters

"Gate, don't trust": a green `Perf PR-fast` currently certifies *no p99/max blowup*, not *within the 6.94 ms steady-state budget*. That gap is invisible to anyone reading the check status, and the calibration that closes it has no owning plan.

## Fix

Tracked in the plan doc (arm `mean_abs_ceiling_ms` + per-scenario overrides from observed CI runs; recapture baselines so p99 + call depth are real; decide whether to deepen scenario frame counts). Tightening a live gate's numbers is a human-judgment call — the plan gates it behind observed-run evidence + user sign-off, never an autonomous flip.

## Self-improvement

Empty.

  Status: applied (2026-07-11 reconcile — verified fixed on develop: docs/perf/regression-policy.json `mean_abs_ceiling_ms` is ARMED at 6.94 (perf-gate step-5 calibration pass, 2026-07-06) with an empty perScenario map; baselines recaptured #1659.)

---

# `test-orphan-bats` runs only in the full suite / CI, not the pre-ship fast path — an unwrapped `.bats` reddens develop a merge later

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [tooling] · P2 — a new bats suite shipped without its `test-*.sh` wrapper; the orphan-bats gate caught it only in CI, on the *next* PR, masking which change introduced the red

## Friction

The mutation-smoke gate slice (#1698) added `tests/bats/mutation_smoke.bats`
without a `test-*.sh` wrapper naming its path. `test-orphan-bats.sh` (which
enforces that every bats suite has a runner, so an added suite can't silently
never-run) **is** auto-enrolled in `scripts/dev/test-all.sh` — verified:
`test-all.sh:113` globs `agents/scripts/core/test-*.sh` and orphan-bats lives
there — but `scripts/dev/pre-ship.sh`, the fast pre-push gate, does **not** run
it (verified: `grep -c orphan scripts/dev/pre-ship.sh` → 0).

So the orphan escaped the local pre-push loop and surfaced only as a red
`Agentic self-tests (bats)` lane on the **next** PR's CI (#1702), one merge after
the change that caused it — the red pointed at an innocent PR and cost a
diagnosis round to trace back to #1698. A trap: the mirror script
`scripts/dev/test-mutation-smoke.sh` *looks* like it covers the suite but it
validates the harness/corpus, not the bats file, so it does not satisfy the
wrapper requirement.

## Proposal

1. Add a fast `bash agents/scripts/core/test-orphan-bats.sh` call to
   `scripts/dev/pre-ship.sh` (the check is near-instant — no build, just a glob +
   grep over wrappers) so an unwrapped suite is caught before push, not a merge
   later on an unrelated PR's CI.
2. Playbook one-liner: **adding a `tests/bats/*.bats` requires its
   `test-<name>-bats.sh` wrapper (naming the suite by `tests/bats/<name>.bats`
   path) in the SAME PR** — a harness/corpus mirror script does not count.

Est ~15 min total. This session fixed the instance by adding
`scripts/dev/test-mutation-smoke-bats.sh` (#1702), but the pre-ship gap remains
and will bite the next suite added without a wrapper.

**Update (2026-07-10): implemented.** Added a
`bash agents/scripts/core/test-orphan-bats.sh` stage to `scripts/dev/pre-ship.sh`
(next to the test-list consistency check), so a wrapper-less bats suite is caught
before push. Archive to `applied.md` on the next sweep.

## Format

- Details: see § Friction. Verified against the committed tree at develop head.
- Concrete next action: see § Proposal (1)–(2) — done.
- Status: applied (stale `Status: open` reconciled during the 2026-08-12 archival sweep — entry already lived in applied.md)
- Last-reviewed: 2026-07-10

  Status: applied (2026-07-11 reconcile — verified fixed on develop: scripts/dev/pre-ship.sh:316 runs `bash agents/scripts/core/test-orphan-bats.sh` in the fast pre-push path.)


<!-- reconcile 2026-07-11: entries below were `Status: applied` in categories/<cat>/ but never moved here; archived in one batch (PR reconcile). -->

- 2026-07-10 · orchestrator (self-improvement campaign ship session) · [process] · P2 — the required `CR findings` status pends forever when CR doesn't produce a review; high-volume campaigns exhaust the adaptive rate-limit (and re-triggers reset it), while docs/self-improvement-only PRs are path-excluded outright — both wedge merge

## Friction

Shipping 11 campaign PRs (#1682–#1692) plus follow-ups in one session pushed
CodeRabbit's per-developer review volume to the 95th percentile, where its
**adaptive** limit releases new reviews only gradually. The repo's required
`CR findings (0 actionable)` status check stays `pending` until CodeRabbit posts
a *completed* review on the PR's current head SHA, so the throttle blocked
#1702's merge for ~2h even though every real CI lane was green and Cursor Bugbot
had already reviewed it with zero actionable findings.

Two behaviours compounded it, both verified this session:

- CodeRabbit **skips draft PRs entirely** — the gate can never satisfy while the
  PR is a draft, so a fix-forward opened as draft sits pending until marked ready.
- Each manual `@coderabbitai review` that lands *inside* an active rate-limit
  window **resets the countdown** — observed the "next review available in" value
  jump from `51 seconds` back up to `38 minutes` immediately after a trigger. So
  re-triggering to "unstick" the gate is actively counterproductive.

The required gate has no degrade path when the external reviewer is unavailable,
so an upstream throttle translates directly into an unbounded merge block.

**Stronger variant, observed on the PR logging this very entry (#1718):** CodeRabbit
**path-excludes** `docs/self-improvement/**` (`!docs/self-improvement/**` in
`.coderabbit.yaml`), so for a docs/self-improvement-only PR it posts "Review skipped
due to path filters" and **never** produces a review. The `CR findings (0 actionable)`
gate is then **structurally unsatisfiable** — no amount of waiting or re-triggering
helps, because there is nothing for CR to review. Same class of failure (CR skips a
draft too), and the fix is the same: the gate must treat "CR will not / cannot review
this PR" (path-excluded, draft-skipped, throttled past a deadline) as **0 findings →
pass**, not perpetual pending.

## Proposal

1. **Agent behaviour (cheap, do first):** when the `CR findings` gate is pending
   due to a CodeRabbit rate-limit, do **not** re-trigger — let the rolling window
   age out, then trigger once. Encode in the PR-babysit / ship-loop playbook next
   to the existing draft-PR note.
2. **Pace campaigns:** stagger PR *readiness* (mark ready in small batches) so CR
   review volume stays under the adaptive limit instead of firing N reviews at once.
3. **Gate design (load-bearing):** the required `CR findings` check must have a
   pass path when CR does not produce a review. Two triggers: (a) an explicit
   **"Review skipped due to path filters"** (or draft-skip) comment from CR on the
   head SHA → treat as 0 findings → **pass immediately** (structural, not a wait);
   (b) after N hours pending with zero findings from any other reviewer
   (Bugbot/Copilot) → degrade to advisory. Without (a), any docs/self-improvement-only
   PR — including the ones this very backlog process produces — can never merge
   without an operator admin-merge.

Est: (1) ~10 min doc; (2) ~15 min playbook; (3) ~1–2h (poller/gate change).
This session resolved #1702 only via an operator-authorized admin merge past the
pending gate.

**Update (2026-07-10): partially implemented (the structural half of proposal 3).**
Ported the **selfImpOnly** terminal pass-signal from the client gate
(`merge-gates.sh`) to the SERVER gate (`.github/actions/cr-finding-gate/action.yml`),
the one that actually blocks merge: a diff entirely under `docs/self-improvement/**`
(path-excluded by `.coderabbit.yaml`, sanctioned by
`self-improvement-pr-review-exemption`) passes immediately, no CR wait — exactly
the docs-only-PR class that wedged. It is head-accurate (queries the PR's current
file list) and fail-closed on any `gh` pagination error.

A second, comment-body-based "terminal path-filter skip" pass was tried and
**dropped after CodeRabbit review** (#1724): CR's skip summary comment carries no
reliable head-commit anchor, so a stale skip comment from an earlier docs-only
commit could pass a LATER code commit before CR re-reviewed it (fail-open race).
selfImpOnly covers the recurring case without that hazard.

Still open (deliberately NOT auto-passed — unsafe): the **rate-limit on a CODE
PR** case. Auto-passing it would wave un-reviewed code through; the correct escape
stays the `cr-out-of-band` label + `cr-disposition:` attestation (already
supported). Proposals (1) don't-re-trigger and (2) pace-campaigns remain doc/
playbook follow-ups.

## Format

- Details: see § Friction. Verified: the rate-limit countdown reset was observed
  in the PR's `coderabbitai[bot]` comments (51s → 38m after a re-trigger); the
  gate context string is `CR findings (0 actionable)` with description
  "awaiting CodeRabbit review on current head".
- Concrete next action: see § Proposal (1)–(3).
- Triggered-follow-up: when=pr-count:base=develop;since=2026-07-10;n=25; action=re-check whether the required CR gate ever degraded gracefully under a throttle, or whether another campaign wedged again; baseline=#1702 blocked ~2h on CR rate-limit despite green CI + Bugbot clear; fired=2026-07-11
- Follow-up observation (2026-07-11): no recurrence. The backlog-takeover session merged five PRs
  (#1726, #1700, #1728, #1730, #1738) while CodeRabbit was continuously rate-limited (its
  "review limit reached" comment present on every PR, windows 15–58 min); the
  `CR findings (0 actionable)` check reached SUCCESS on each head within the normal CI window and
  every merge proceeded without an admin-merge or `cr-out-of-band` label. The remaining unsafe
  case (rate-limit wedging a code PR past its window) did not reproduce; proposals (1)/(2) stay
  open as playbook follow-ups.
- Update (2026-08-13): proposals (1) and (2) landed as the "CodeRabbit rate-limit
  playbook" in `docs/agent-rules/merge-gates.md` (never re-trigger inside an active
  window — the countdown resets; stagger campaign PR readiness; per-PR, batch fix
  rounds into one push per the pre-first-push gate). The structural half of (3)
  (selfImpOnly terminal pass) shipped 2026-07-10; the throttled-code-PR case stays a
  deliberate BLOCK with the `cr-out-of-band` + `cr-disposition:` escape, per the
  2026-07-11 follow-up observation that it never recurred across five rate-limited
  merges. Separately, the CI gate now auto-posts a recency-gated
  `@coderabbitai full review` nudge when a COMPLETED clean pass leaves no on-head
  evidence (PR #2004) — the self-heal for the stale-evidence wedge family this
  entry first recorded. Nothing remains open.
- Status: applied
- Last-reviewed: 2026-08-13

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [test] · P2 — MCP live-HTTP `Authorize` path (DNS-rebind gate, SSE cap) is tested only via pure helpers, never over a real socket
  Details: `IsMcpHostOriginAllowed`, `ConstantTimeStringEquals`, and the SSE-cap predicate have solid doctest coverage (tests/Plugins/Mcp/), but no test drives `McpPlugin::Authorize` over a real `httplib` connection with a hostile `Host:`/`Origin:` header, a missing/wrong token, or a race on the SSE connection cap — the layer where the route registration order and header plumbing could silently diverge from the pure helpers. The repo already owns the exact fixture shape: `tests/support/JiraCatalogHttpFixture.h` runs an in-process httplib loopback server against real cpr. AGENTIC_INFRA_AUDIT.md finding C6; corroborates TEST_COVERAGE_GAP_MAP.md (Plugins/Mcp is 5 TUs).
  Concrete next action: add an integration TU that starts `McpPlugin` on an ephemeral loopback port and asserts over real HTTP: 403 on non-loopback Host, 403 on cross-origin Origin, 401 without token when `McpRequireTokenOnLoopback`, 200 with token, and 503 past the SSE cap. Effort M.
  Resolution: SHIPPED (2026-07-13, agentic-infra-audit-review PR) — bucket-E TU `tests/ui/mcp_live_http_auth.test.cpp` (test `McpLiveHttp/Authorize_RealSocket`) starts a SECOND `McpPlugin` on its own port (constructing/OnStart-ing it directly, so it never restarts the rig's own plugin that the parent CLI is driving over MCP) with the secure defaults (loopback bind, token set, `require_token_on_loopback` ON) and asserts over a real `httplib::Client`: 200 with a valid token + tools/list body, 401 without / with a wrong token (+ WWW-Authenticate), 403 on a DNS-rebind `Host:` even WITH a valid token (Host gate precedes the token check; cpp-httplib v0.49 honours a caller-supplied Host), 403 on a cross-origin `Origin:`, and 503 once `kMaxConcurrentSseConnections` (4) SSE streams are held open. A RAII fixture joins the SSE-holder threads, stops the test server, and restores both the persisted config (OnStart re-reads the token) and instance.json (OnStart overwrites / OnStop deletes the rig's discovery file). Registered in `tests/ui/ui_tests_registry.cpp` under `#if defined(SMATCHET_WITH_MCP)`, enrolled in `tests/ui/CMakeLists.txt`, driver `scripts/dev/test-ui-mcp-live-http-auth.sh` (zero-match fail-closed guard; auto-discovered by `test-all.sh`).
  Status: applied — CI-VERIFIED 2026-07-13 on the `Bucket-E UI tests (Mesa headless GL)` lane (PR #1812, commit 1547763): `McpLiveHttp/Authorize_RealSocket` builds and passes all six assertions. Environment-parity postscript (finding C3, confirmed the hard way): the authoring session ran in a Linux container that cannot build the bucket-E rig, so the TU shipped code-complete-but-unrun — and CI then caught TWO MSVC `/W4 /WX` warnings the container was blind to, each costing a fix + CI round-trip: (1) `C2446` — `res != nullptr` on an `httplib::Result` (non-explicit `operator bool` wins overload resolution → `int != nullptr`), fixed by asserting `res.error() == httplib::Error::Success`; (2) `C4456` — the ImGui-Test-Engine `IM_CHECK` macro internally declares a `bool res` that shadowed the local `httplib::Result res`, fixed by renaming the local to `httpRes`. Neither is reproducible off a bucket-E-capable toolchain; both are exactly why C3 (declared capability tiers so a Linux agent knows what it cannot self-verify) matters. The regular `Windows + MSVC` lane is NOT sufficient coverage — it does not compile `tests/ui/` (opt-in `SMATCHET_BUILD_UI_TESTS`); only the bucket-E lanes do. PC/local re-run steps remain in [`docs/plans/shipped/pc-verify-agentic-audit-followups.md`](../../plans/shipped/pc-verify-agentic-audit-followups.md) Task A.
  Last-reviewed: 2026-07-13

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [process] · P2 — AI_POLICY.md promises an automated cost-ceiling gate that was descoped and never re-tracked
  Details: `AI_POLICY.md` § Cost control stated the automated cost-ceiling gate is "not yet built"; the shipped charter plan (`docs/plans/ai-control-policy.md` § Out of scope) descoped it to "a follow-up (pairs with token-tracking)" and no live tracker carried it since. AGENTIC_INFRA_AUDIT.md finding A6.
  Resolution: applied (2026-07-09, audit-followups PR #1680 — A6-only after B1 landed separately on develop via #1686) — built option (a), the gate, in the WARN-first idiom: `agents/scripts/core/cost-ceiling-check.py` (with `--selftest` incl. malformed-config/non-dict-row fail-open cases; `--blocking` reserved for graduation) sums input+output tokens from the token-tracking JSONL and prints an ESCALATE banner at/over `project.config.json` § `governance.session_token_ceiling` (default 5000000; 0 disables); SessionStart wrapper `cost-ceiling-nudge.sh` wired into `docs/harness/claude-code/settings.json.tmpl`; `test-cost-ceiling-check.sh` auto-enrolls in test-all.sh; AI_POLICY.md § Cost control now describes the shipped advisory backstop instead of promising one.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-06 · claude-code (perf-gate step-5 session) · [infra] · P2 — perf-full's gh/git steps lacked `shell: bash` → scheduled full-suite perpetually RED (silent); auto-issue/auto-PR mechanisms dead
  Details: on `windows-2022` a `run:` step with no `shell:` defaults to PowerShell; perf-full.yml's three follow-up steps (scenario-run-failure issue / regression issue / baseline-bump PR) used bash syntax and crashed whenever they fired — and they fired every run because ~8 non-baselined scenarios always fail to spawn, so the scheduled suite was RED for ≥ a week unnoticed and the auto-issue/auto-PR mechanisms never actually ran. A naive `shell: bash` fix alone would have spammed one issue per run (per-run-id title), and the improvement-bump `gh pr create` hits the repo's "Actions may not create PRs" setting. Full analysis is in the original entry file (git history: `docs/self-improvement/categories/infra/2026-07-06-perf-full-steps-missing-shell-bash-perpetual-red.md`).
  Resolution: applied — #1681 (`51989b6`) closed the remaining in-tree gaps: `shell: bash` on all steps (interim commits), "Discover scenarios" intersects `scenario.list` with the committed baseline set (`git ls-files docs/perf/baselines/*.ci-windows-latest.json`) so `run_failure_count` only counts real in-scope breaks, both issue steps are idempotent (stable title + find-then-comment), and the improvement bump is push-only (drops the blocked `gh pr create`). The 8 spawn failures are confirmed expected non-perf-runnable (screenshot-required / test-engine / not-a-perf-scenario), not broken.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [process] · P2 — AGENTS.md is 159 lines against its own ≤150 contract budget (grandfathered, never trims)
  Details: `AGENTS.md` declares `contract_budget_lines: 150` and the `agent-too-long` lint enforces that token — but the file is 159 lines and `agent_size_audit.py`'s delta gate grandfathers keys already over-cap at the merge base, so the violation persists indefinitely and even growth never fires. The doc that anchors the enforcement contract-card being durably over its own budget is the self-description-drift class in miniature. AGENTIC_INFRA_AUDIT.md finding A1.
  Concrete next action: judgment trim, not mechanical — extract detail-heavy prose (inline PR-number citations, per-exception detail already duplicated in `docs/agent-rules/ship-loops.md`) into the pointed-to `docs/agent-rules/` docs until AGENTS.md is ≤150 lines; then consider a one-time baseline refresh so the cap becomes binding again for this key. Effort M.
  Resolution: applied — AGENTS.md trimmed 159 → 149 lines (merge-throughput paragraph moved to merge-gates.md, auto-merge/red-check prose condensed onto merge-gates.md pointers, § Semantic-search exceptions + caveman sections folded to bold-prefix paragraphs; every anchor kept, test-doc-anchors green) and the agent-size baseline refreshed (`--agentsize-baseline`; AGENTS.md key no longer grandfathered, cap binding again).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · claude (AppController extraction session) · [tooling] · P2 — `include-curation-freefunction-false-negative`: when splitting a TU into a companion `.cpp` in an environment where no Core TU compiles locally (curl/cpr fetch blocked by egress policy → `posix-core-check` can't even configure), curating the new TU's includes by a symbol-usage heuristic keyed on *type/class tokens* silently drops a header whose only use is a free function — a CI-only compile failure.
  Details: Slice 1 of the AppController cluster extraction (PR #1653) curated `AppController_Init.cpp`'s includes down from a superset (the superset tripped the blocking DRY duplication gate). The trim heuristic checked each candidate header by searching the moved body for a representative *type* name — e.g. `Ui/SmatchetFieldRender.h` was probed for `FieldRender` (0 hits) and dropped. But `RunLegacyStartupSweeps` calls the *free function* `SetCallstackFieldIdHint` declared in that header, so the drop produced `error: use of undeclared identifier 'SetCallstackFieldIdHint'`. Because AppController.cpp needs cpr/curl (blocked here), nothing compiled locally; the error surfaced only on CI — first on the fast `Mobile — Android emulator smoke` lane (~1 min), then Windows MSVC light/ARM64 and Perf. One-commit fix (`4101155`) restored the header; cost ≈ one CI round-trip (~10 min latency).
  Concrete next action (low urgency; process fix, no code owed): when curating a companion-TU include set without a local compiler, verify inclusion against BOTH (a) type/class/enum names AND (b) *every* `CapitalizedIdentifier(` free-function call site and every `ns::Func(` namespace-qualified call in the moved body, mapping each to its declaring header — this is what Slice 2 (`AppController_PaneContexts.cpp`) then did and it landed clean with zero round-trips. Candidate durable home: a one-liner in `docs/agent-rules/cpp-rules.md` § File-split (the post-split include-replication rule) noting "curate against free-function call sites too, not just types — a type-only grep gives false negatives that only CI catches when the TU can't compile locally." Alternatively, prefer the full-superset-plus-`duplication`-deviation approach when local compile is impossible and CI latency is the binding cost (guarantees compile, trades one dup exemption for zero round-trips).
  Resolution: applied — one-liner added to docs/agent-rules/cpp-rules.md § File size (the file-split recipe): curate companion-TU includes against BOTH type/enum names AND every CapitalizedIdentifier( / ns::Func( free-function call site when no local compiler is available, or keep the full superset + a duplication deviation.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [tooling] · P3 — `tools/sourcetrail/st_query.py` is documented as the primary semantic-nav tool but needs a prebuilt DB absent from fresh checkouts
  Details: AGENTS.md sells `st_query.py` as the first stop before grep, but Sourcetrail is discontinued upstream and the required symbol DB is neither in the repo nor buildable by any checked-in script — in a fresh clone (and in every Linux container session) the "primary" nav tool is a no-op with extra steps. A rulebook recommending a tool that cannot run erodes trust in its other recommendations. AGENTIC_INFRA_AUDIT.md finding C7 / proposal P9.
  Concrete next action: pick one: (a) retire — remove `tools/sourcetrail/` and the AGENTS.md claim, leaving grep + compile_commands-based tooling as the documented path; or (b) re-bootstrap — replace with a `clangd`-index-backed query script (clangd is alive and `compile_commands.json` already exists per preset) and update the rulebook pointer. Either way, stop documenting the dead path. Effort S (retire) / M (replace).
  Resolution: applied — option (a) retire: tools/sourcetrail/ deleted; the Sourcetrail rung removed from the AGENTS.md § Semantic codebase search precedence ladder, docs/harness/claude-code/CLAUDE.md.tmpl, docs/harness/capability-adapter.md, and docs/CONTEXT.md; AGENTIC_INFRA_AUDIT.md finding C7 marked remediated.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [tooling] · P3 — `tools/repo-health/facts.json` rots silently between sessions; the dashboard shows stale gate states with no freshness signal
  Details: the repo-health dashboard splits "computed" metrics (recomputed every run) from "facts" (CI lane statuses, PR gate states, campaign verdicts) that are session-maintained in `facts.json` because the generator cannot reach GitHub — its own README admits the rot risk. A dashboard rendering weeks-old gate states as current is worse than no dashboard for the human-on-the-loop visibility role AI_POLICY.md assigns it. AGENTIC_INFRA_AUDIT.md finding C8.
  Concrete next action: (a) stamp each fact with a `last-updated` date and render age prominently (e.g. amber >7 days, red >30) in `generate.py`/`template.html`; (b) add a SessionStart nudge (pattern: `followup-due-nudge.sh`) that fires when `facts.json` is older than a threshold, prompting a refresh pass. Effort S.
  Resolution: applied — facts.json gained a per-section `updated` stamp map; generate.py/template.html render the oldest stamp as a header freshness badge (green ≤7d / amber ≤30d / red beyond); new SessionStart nudge `agents/scripts/core/repo-health-facts-nudge.sh` (wired into both hook templates, bats-covered) nags when facts.json's git-commit age exceeds 7 days.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [debt] · P3 — `project.config.json` duplicates the 24-item required-checks list verbatim across `branch_protection.required_contexts` and `ci.required_checks`
  Details: the two arrays are identical, and `test-required-context-parity.sh` guards them against divergence — so this is guarded duplication, not the unguarded-drift class. Still, in the value table that anchors a DRY-enforcing project (Engineering Pillar 5 is a blocking gate), deriving one list from the other would delete both the duplication and the guard that exists only to police it. AGENTIC_INFRA_AUDIT.md finding A5.
  Concrete next action: keep `branch_protection.required_contexts` as the single source; make `ci.required_checks` consumers read the branch_protection list (via `scripts/dev/project-config.sh` / the schema), or replace the second array with a `"same-as": "branch_protection.required_contexts"` sentinel the schema validates; retire the parity gate once no second literal list exists. Check consumers of both keys before the cut. Effort S.
  Resolution: applied — `ci.required_checks` deleted from project.config.json (branch_protection.required_contexts is the single source); project-config.sh derives `CI_REQUIRED_CHECKS` from it (its own emit was the sole consumer, with zero downstream readers); the schema now requires only `ci.path_filters` and its `additionalProperties:false` rejects a reintroduced second list. Note: the entry's parity-guard claim was stale — test-required-context-parity.sh validates required_contexts against the workflows and never compared the two arrays, so the duplication was in fact unguarded; that gate stays (it guards a different property and passes 22/22 post-cut).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [infra] · P2 — fresh-clone bootstrap hole: every session hook/guard is inert until `setup-harness.sh` runs, and only a manual probe warns
  Details: the `.claude/` adapter dir (hooks, guards, settings) is gitignored and provisioned only by `agents/scripts/core/setup-harness.sh`; in a fresh clone the head-drift, plan-lock, and shared-tree guards plus every SessionStart nudge are silently absent. `check-harness-provisioned.sh` exists to surface this but must be invoked by hand. `docs/plans/session-guard-agnostic.md` names the fresh-clone gap as an explicit non-goal ("their own in-flight effort") — but no live tracker actually carries it. AGENTIC_INFRA_AUDIT.md finding C5.
  Concrete next action: (a) fold `check-harness-provisioned.sh` into `scripts/dev/doctor.sh` so the standard preflight reports the unprovisioned state; (b) add a cheap self-check to the git `pre-push` hook path (already repo-owned, so it *does* run in fresh clones) that warns when `.claude/hooks/` is absent under a Claude-harness session. Effort S.
  Resolution: applied — slice (a): `doctor.sh` now runs `check-harness-provisioned.sh --quiet` as a warn-only preflight check (`[WARN] harness` unprovisioned / `[PASS] harness` wired; covered by `tests/bats/harness_provisioned_doctor.bats`). Slice (b)'s premise was wrong: `scripts/git-hooks/pre-push` is itself only wired via `core.hooksPath` BY `setup-harness.sh`, so no git hook runs in a fresh clone either — replaced with a doc note in `docs/harness/SETUP.md` § Check anytime stating that fact and pointing at the doctor probe.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P1 — AI assistant auto-context bodies are injected into the system prompt unsanitized (prompt-injection surface)
  Details: `ComposeSystemPrompt` (AiAssistantController.h) wraps each auto-context block in `<smatchet_context block="...">` tags and XML-escapes only the *attribute*; the *body* — ticket summaries, labels, audit-trail strings, visible grid rows, all attacker-influenceable via the tracker backend — is inserted verbatim. A malicious ticket summary can attempt closing-tag breakout or instruction injection into the model. The outbound-consent modal mitigates exfil *volume* (real byte counts) but shows sizes, not content, and does nothing against instruction injection. AGENTIC_INFRA_AUDIT.md finding B1.
  Concrete next action: (a) escape/neutralize `</smatchet_context` sequences in block bodies before assembly (pure helper, unit-testable in the existing tests/Core/AiAssistantSystemPrompt TU); (b) append one fixed line to the composed system prompt stating that content inside `smatchet_context` tags is data from the tracker, never instructions. Effort S.
  Resolution: applied — `NeutralizeContextBody` (AiXmlAttrEscape.h, pure) breaks `<smatchet_context`/`</smatchet_context` sequences in block bodies (`&lt;` on the leading `<`) at both assembly sites (`ComposeSystemPrompt` + `AiContextBuilder::AppendBlock`), and `ContextDataNotInstructionsLine()` adds the fixed data-not-instructions sentence after the context header; covered in tests/Core/AiAssistantSystemPrompt.test.cpp (breakout neutralized, benign unchanged, preamble iff blocks).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P2 — MCP `tools/call` has no rate limit; only SSE connection count is bounded
  Details: every MCP `tools/call` (JSON-RPC and the REST equivalent) dispatches into the command registry with bounded parsing and destructive gating, but no frequency bound — a buggy or hostile local client can hot-loop non-destructive commands (`tickets.search*`, `perf.dump`, ...) unthrottled. `CanAcceptSseConnection` bounds SSE streams (503 over-cap) but nothing bounds tool-call rate. Distinct from the archived "MCP registry dispatch un-gated after Authorize" entry (its destructive-confirm half shipped in PR #1246; its residual is capability *scoping*, not rate). AGENTIC_INFRA_AUDIT.md finding B3.
  Concrete next action: add a token-bucket at `DispatchRegistryToolsCall` in `Source/Plugins/Mcp/McpPlugin.cpp` (one chokepoint covers JSON-RPC + REST + legacy routes); return a structured `rate-limited` error envelope; make bucket size/refill configurable via `TrackerConfig` with a sane default; extract the decision to a pure helper for doctest coverage. Effort M.
  Resolution: applied — `ConsumeToolsCallToken` (McpRateLimitPure.h, pure token bucket, doctested in tests/Plugins/Mcp/McpRateLimit.test.cpp) gates both real entry points — REST `HandleToolsCall` and JSON-RPC `HandleJsonRpcToolsCall` (the JSON-RPC path does NOT funnel through `DispatchRegistryToolsCall`, so the gate sits one level up and covers every dispatch arm incl. run_lua/Lua tools/legacy) — sharing one bucket; deny returns the canonical HTTP-200 `rate-limited` envelope (REST) / JSON-RPC -32000 with retry-after; `TrackerConfig::McpToolsCallRateBurst`/`RateRefillPerSec` (default 20 burst / 5 per s, <=0 disables) persist as `mcp_tools_call_rate_*` and participate in `NeedsRestart`.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P2 — debug `ai.dump-request` path re-implements AI client config/URL building and skips the production sanitizers
  Details: PARTIALLY LANDED (2026-07-08, backlog batch security-ai-mcp): the config half is unified — `SanitizeHeaderValue` + `BuildClientConfig` (key sanitizing, base-URL fallback chains, `EndpointPolicyForProvider` sanitize-with-consent gate, streaming timeout) moved from `AiAssistantController.cpp`'s anonymous namespace to the shared seam `Source/Core/src/AiRequestBuilder.cpp` (+ header), now consumed by the controller AND all three debug call sites (`ai.dump-request` / `ai.probe` / `ai.send-once`); the `BuildClientConfigForProvider` clone in `BuiltinCommands_Ai.cpp` is deleted, so the debug path no longer skips the sanitizers (doctested in tests/Core/AiRequestBuilder.test.cpp). REMAINING: the debug body/URL builders (`BuildAnthropicBody`/`BuildOpenAiBody`/`BuildOllamaNativeBody`/`ResolveEndpointUrl`/`StripOpenAiV1Suffix` in `BuiltinCommands_Ai.cpp`) still mirror the per-client `BuildChatBody`/`ResolveBaseUrl`/`JoinUrl` (anonymous namespaces in OpenAiClient/AnthropicClient/OllamaClient.cpp) instead of calling them — the residual drift surface. The archived 2026-05-17 entry records `ai.dump-request` already misreporting the wire once (fixed post-PR #184). AGENTIC_INFRA_AUDIT.md finding B4 / proposal P4.
  Concrete next action: expose the per-client body/URL builders (the `OllamaBuildRequestBodyJson` pattern already exists in OllamaClient.cpp) and make `ai.dump-request` call them, deleting the debug mirrors; then add doctest coverage asserting the debug dump equals the production wire for each provider.
  Status: applied (2026-07-11 — the remaining drift surface is closed: new `AiWireIntrospect.h` exposes `smatchet::ai::{OpenAi,Anthropic,OllamaNative}BuildChatBodyJson` + `...ResolveChatUrl`, each a thin wrapper over the SAME anonymous-namespace `BuildChatBody`/`ResolveBaseUrl`/`JoinUrl` the live client dispatch uses. `ai.dump-request` builds an `AiChatRequest` and calls them; the `BuildAnthropicBody`/`BuildOpenAiBody`/`BuildOllamaNativeBody`/`ResolveEndpointUrl`/`StripOpenAiV1Suffix` mirrors in BuiltinCommands_Ai.cpp are deleted. Because the dump now shares the production builder, it can no longer drift OR drop history (the mirrors only ever emitted a single user turn). Doctest `tests/Core/AiWireIntrospect.test.cpp` locks the per-provider wire shape incl. the full system+multi-turn body. Dual-target compiled.)
  Last-reviewed: 2026-07-11

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [test] · P2 — MCP live-HTTP `Authorize` path (DNS-rebind gate, SSE cap) is tested only via pure helpers, never over a real socket
  Details: `IsMcpHostOriginAllowed`, `ConstantTimeStringEquals`, and the SSE-cap predicate have solid doctest coverage (tests/Plugins/Mcp/), but no test drives `McpPlugin::Authorize` over a real `httplib` connection with a hostile `Host:`/`Origin:` header, a missing/wrong token, or a race on the SSE connection cap — the layer where the route registration order and header plumbing could silently diverge from the pure helpers. The repo already owns the exact fixture shape: `tests/support/JiraCatalogHttpFixture.h` runs an in-process httplib loopback server against real cpr. AGENTIC_INFRA_AUDIT.md finding C6; corroborates TEST_COVERAGE_GAP_MAP.md (Plugins/Mcp is 5 TUs).
  Concrete next action: add an integration TU that starts `McpPlugin` on an ephemeral loopback port and asserts over real HTTP: 403 on non-loopback Host, 403 on cross-origin Origin, 401 without token when `McpRequireTokenOnLoopback`, 200 with token, and 503 past the SSE cap. Effort M.
  Resolution: SHIPPED (2026-07-13, agentic-infra-audit-review PR) — bucket-E TU `tests/ui/mcp_live_http_auth.test.cpp` (test `McpLiveHttp/Authorize_RealSocket`) starts a SECOND `McpPlugin` on its own port (constructing/OnStart-ing it directly, so it never restarts the rig's own plugin that the parent CLI is driving over MCP) with the secure defaults (loopback bind, token set, `require_token_on_loopback` ON) and asserts over a real `httplib::Client`: 200 with a valid token + tools/list body, 401 without / with a wrong token (+ WWW-Authenticate), 403 on a DNS-rebind `Host:` even WITH a valid token (Host gate precedes the token check; cpp-httplib v0.49 honours a caller-supplied Host), 403 on a cross-origin `Origin:`, and 503 once `kMaxConcurrentSseConnections` (4) SSE streams are held open. A RAII fixture joins the SSE-holder threads, stops the test server, and restores both the persisted config (OnStart re-reads the token) and instance.json (OnStart overwrites / OnStop deletes the rig's discovery file). Registered in `tests/ui/ui_tests_registry.cpp` under `#if defined(SMATCHET_WITH_MCP)`, enrolled in `tests/ui/CMakeLists.txt`, driver `scripts/dev/test-ui-mcp-live-http-auth.sh` (zero-match fail-closed guard; auto-discovered by `test-all.sh`).
  Status: applied — CI-VERIFIED 2026-07-13 on the `Bucket-E UI tests (Mesa headless GL)` lane (PR #1812, commit 1547763): `McpLiveHttp/Authorize_RealSocket` builds and passes all six assertions. Environment-parity postscript (finding C3, confirmed the hard way): the authoring session ran in a Linux container that cannot build the bucket-E rig, so the TU shipped code-complete-but-unrun — and CI then caught TWO MSVC `/W4 /WX` warnings the container was blind to, each costing a fix + CI round-trip: (1) `C2446` — `res != nullptr` on an `httplib::Result` (non-explicit `operator bool` wins overload resolution → `int != nullptr`), fixed by asserting `res.error() == httplib::Error::Success`; (2) `C4456` — the ImGui-Test-Engine `IM_CHECK` macro internally declares a `bool res` that shadowed the local `httplib::Result res`, fixed by renaming the local to `httpRes`. Neither is reproducible off a bucket-E-capable toolchain; both are exactly why C3 (declared capability tiers so a Linux agent knows what it cannot self-verify) matters. The regular `Windows + MSVC` lane is NOT sufficient coverage — it does not compile `tests/ui/` (opt-in `SMATCHET_BUILD_UI_TESTS`); only the bucket-E lanes do. PC/local re-run steps remain in [`docs/plans/shipped/pc-verify-agentic-audit-followups.md`](../../plans/shipped/pc-verify-agentic-audit-followups.md) Task A.
  Last-reviewed: 2026-07-13

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [process] · P2 — AI_POLICY.md promises an automated cost-ceiling gate that was descoped and never re-tracked
  Details: `AI_POLICY.md` § Cost control stated the automated cost-ceiling gate is "not yet built"; the shipped charter plan (`docs/plans/ai-control-policy.md` § Out of scope) descoped it to "a follow-up (pairs with token-tracking)" and no live tracker carried it since. AGENTIC_INFRA_AUDIT.md finding A6.
  Resolution: applied (2026-07-09, audit-followups PR #1680 — A6-only after B1 landed separately on develop via #1686) — built option (a), the gate, in the WARN-first idiom: `agents/scripts/core/cost-ceiling-check.py` (with `--selftest` incl. malformed-config/non-dict-row fail-open cases; `--blocking` reserved for graduation) sums input+output tokens from the token-tracking JSONL and prints an ESCALATE banner at/over `project.config.json` § `governance.session_token_ceiling` (default 5000000; 0 disables); SessionStart wrapper `cost-ceiling-nudge.sh` wired into `docs/harness/claude-code/settings.json.tmpl`; `test-cost-ceiling-check.sh` auto-enrolls in test-all.sh; AI_POLICY.md § Cost control now describes the shipped advisory backstop instead of promising one.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-06 · claude-code (perf-gate step-5 session) · [infra] · P2 — perf-full's gh/git steps lacked `shell: bash` → scheduled full-suite perpetually RED (silent); auto-issue/auto-PR mechanisms dead
  Details: on `windows-2022` a `run:` step with no `shell:` defaults to PowerShell; perf-full.yml's three follow-up steps (scenario-run-failure issue / regression issue / baseline-bump PR) used bash syntax and crashed whenever they fired — and they fired every run because ~8 non-baselined scenarios always fail to spawn, so the scheduled suite was RED for ≥ a week unnoticed and the auto-issue/auto-PR mechanisms never actually ran. A naive `shell: bash` fix alone would have spammed one issue per run (per-run-id title), and the improvement-bump `gh pr create` hits the repo's "Actions may not create PRs" setting. Full analysis is in the original entry file (git history: `docs/self-improvement/categories/infra/2026-07-06-perf-full-steps-missing-shell-bash-perpetual-red.md`).
  Resolution: applied — #1681 (`51989b6`) closed the remaining in-tree gaps: `shell: bash` on all steps (interim commits), "Discover scenarios" intersects `scenario.list` with the committed baseline set (`git ls-files docs/perf/baselines/*.ci-windows-latest.json`) so `run_failure_count` only counts real in-scope breaks, both issue steps are idempotent (stable title + find-then-comment), and the improvement bump is push-only (drops the blocked `gh pr create`). The 8 spawn failures are confirmed expected non-perf-runnable (screenshot-required / test-engine / not-a-perf-scenario), not broken.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [process] · P2 — AGENTS.md is 159 lines against its own ≤150 contract budget (grandfathered, never trims)
  Details: `AGENTS.md` declares `contract_budget_lines: 150` and the `agent-too-long` lint enforces that token — but the file is 159 lines and `agent_size_audit.py`'s delta gate grandfathers keys already over-cap at the merge base, so the violation persists indefinitely and even growth never fires. The doc that anchors the enforcement contract-card being durably over its own budget is the self-description-drift class in miniature. AGENTIC_INFRA_AUDIT.md finding A1.
  Concrete next action: judgment trim, not mechanical — extract detail-heavy prose (inline PR-number citations, per-exception detail already duplicated in `docs/agent-rules/ship-loops.md`) into the pointed-to `docs/agent-rules/` docs until AGENTS.md is ≤150 lines; then consider a one-time baseline refresh so the cap becomes binding again for this key. Effort M.
  Resolution: applied — AGENTS.md trimmed 159 → 149 lines (merge-throughput paragraph moved to merge-gates.md, auto-merge/red-check prose condensed onto merge-gates.md pointers, § Semantic-search exceptions + caveman sections folded to bold-prefix paragraphs; every anchor kept, test-doc-anchors green) and the agent-size baseline refreshed (`--agentsize-baseline`; AGENTS.md key no longer grandfathered, cap binding again).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · claude (AppController extraction session) · [tooling] · P2 — `include-curation-freefunction-false-negative`: when splitting a TU into a companion `.cpp` in an environment where no Core TU compiles locally (curl/cpr fetch blocked by egress policy → `posix-core-check` can't even configure), curating the new TU's includes by a symbol-usage heuristic keyed on *type/class tokens* silently drops a header whose only use is a free function — a CI-only compile failure.
  Details: Slice 1 of the AppController cluster extraction (PR #1653) curated `AppController_Init.cpp`'s includes down from a superset (the superset tripped the blocking DRY duplication gate). The trim heuristic checked each candidate header by searching the moved body for a representative *type* name — e.g. `Ui/SmatchetFieldRender.h` was probed for `FieldRender` (0 hits) and dropped. But `RunLegacyStartupSweeps` calls the *free function* `SetCallstackFieldIdHint` declared in that header, so the drop produced `error: use of undeclared identifier 'SetCallstackFieldIdHint'`. Because AppController.cpp needs cpr/curl (blocked here), nothing compiled locally; the error surfaced only on CI — first on the fast `Mobile — Android emulator smoke` lane (~1 min), then Windows MSVC light/ARM64 and Perf. One-commit fix (`4101155`) restored the header; cost ≈ one CI round-trip (~10 min latency).
  Concrete next action (low urgency; process fix, no code owed): when curating a companion-TU include set without a local compiler, verify inclusion against BOTH (a) type/class/enum names AND (b) *every* `CapitalizedIdentifier(` free-function call site and every `ns::Func(` namespace-qualified call in the moved body, mapping each to its declaring header — this is what Slice 2 (`AppController_PaneContexts.cpp`) then did and it landed clean with zero round-trips. Candidate durable home: a one-liner in `docs/agent-rules/cpp-rules.md` § File-split (the post-split include-replication rule) noting "curate against free-function call sites too, not just types — a type-only grep gives false negatives that only CI catches when the TU can't compile locally." Alternatively, prefer the full-superset-plus-`duplication`-deviation approach when local compile is impossible and CI latency is the binding cost (guarantees compile, trades one dup exemption for zero round-trips).
  Resolution: applied — one-liner added to docs/agent-rules/cpp-rules.md § File size (the file-split recipe): curate companion-TU includes against BOTH type/enum names AND every CapitalizedIdentifier( / ns::Func( free-function call site when no local compiler is available, or keep the full superset + a duplication deviation.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [tooling] · P3 — `tools/sourcetrail/st_query.py` is documented as the primary semantic-nav tool but needs a prebuilt DB absent from fresh checkouts
  Details: AGENTS.md sells `st_query.py` as the first stop before grep, but Sourcetrail is discontinued upstream and the required symbol DB is neither in the repo nor buildable by any checked-in script — in a fresh clone (and in every Linux container session) the "primary" nav tool is a no-op with extra steps. A rulebook recommending a tool that cannot run erodes trust in its other recommendations. AGENTIC_INFRA_AUDIT.md finding C7 / proposal P9.
  Concrete next action: pick one: (a) retire — remove `tools/sourcetrail/` and the AGENTS.md claim, leaving grep + compile_commands-based tooling as the documented path; or (b) re-bootstrap — replace with a `clangd`-index-backed query script (clangd is alive and `compile_commands.json` already exists per preset) and update the rulebook pointer. Either way, stop documenting the dead path. Effort S (retire) / M (replace).
  Resolution: applied — option (a) retire: tools/sourcetrail/ deleted; the Sourcetrail rung removed from the AGENTS.md § Semantic codebase search precedence ladder, docs/harness/claude-code/CLAUDE.md.tmpl, docs/harness/capability-adapter.md, and docs/CONTEXT.md; AGENTIC_INFRA_AUDIT.md finding C7 marked remediated.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [tooling] · P3 — `tools/repo-health/facts.json` rots silently between sessions; the dashboard shows stale gate states with no freshness signal
  Details: the repo-health dashboard splits "computed" metrics (recomputed every run) from "facts" (CI lane statuses, PR gate states, campaign verdicts) that are session-maintained in `facts.json` because the generator cannot reach GitHub — its own README admits the rot risk. A dashboard rendering weeks-old gate states as current is worse than no dashboard for the human-on-the-loop visibility role AI_POLICY.md assigns it. AGENTIC_INFRA_AUDIT.md finding C8.
  Concrete next action: (a) stamp each fact with a `last-updated` date and render age prominently (e.g. amber >7 days, red >30) in `generate.py`/`template.html`; (b) add a SessionStart nudge (pattern: `followup-due-nudge.sh`) that fires when `facts.json` is older than a threshold, prompting a refresh pass. Effort S.
  Resolution: applied — facts.json gained a per-section `updated` stamp map; generate.py/template.html render the oldest stamp as a header freshness badge (green ≤7d / amber ≤30d / red beyond); new SessionStart nudge `agents/scripts/core/repo-health-facts-nudge.sh` (wired into both hook templates, bats-covered) nags when facts.json's git-commit age exceeds 7 days.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [debt] · P3 — `project.config.json` duplicates the 24-item required-checks list verbatim across `branch_protection.required_contexts` and `ci.required_checks`
  Details: the two arrays are identical, and `test-required-context-parity.sh` guards them against divergence — so this is guarded duplication, not the unguarded-drift class. Still, in the value table that anchors a DRY-enforcing project (Engineering Pillar 5 is a blocking gate), deriving one list from the other would delete both the duplication and the guard that exists only to police it. AGENTIC_INFRA_AUDIT.md finding A5.
  Concrete next action: keep `branch_protection.required_contexts` as the single source; make `ci.required_checks` consumers read the branch_protection list (via `scripts/dev/project-config.sh` / the schema), or replace the second array with a `"same-as": "branch_protection.required_contexts"` sentinel the schema validates; retire the parity gate once no second literal list exists. Check consumers of both keys before the cut. Effort S.
  Resolution: applied — `ci.required_checks` deleted from project.config.json (branch_protection.required_contexts is the single source); project-config.sh derives `CI_REQUIRED_CHECKS` from it (its own emit was the sole consumer, with zero downstream readers); the schema now requires only `ci.path_filters` and its `additionalProperties:false` rejects a reintroduced second list. Note: the entry's parity-guard claim was stale — test-required-context-parity.sh validates required_contexts against the workflows and never compared the two arrays, so the duplication was in fact unguarded; that gate stays (it guards a different property and passes 22/22 post-cut).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [infra] · P2 — fresh-clone bootstrap hole: every session hook/guard is inert until `setup-harness.sh` runs, and only a manual probe warns
  Details: the `.claude/` adapter dir (hooks, guards, settings) is gitignored and provisioned only by `agents/scripts/core/setup-harness.sh`; in a fresh clone the head-drift, plan-lock, and shared-tree guards plus every SessionStart nudge are silently absent. `check-harness-provisioned.sh` exists to surface this but must be invoked by hand. `docs/plans/session-guard-agnostic.md` names the fresh-clone gap as an explicit non-goal ("their own in-flight effort") — but no live tracker actually carries it. AGENTIC_INFRA_AUDIT.md finding C5.
  Concrete next action: (a) fold `check-harness-provisioned.sh` into `scripts/dev/doctor.sh` so the standard preflight reports the unprovisioned state; (b) add a cheap self-check to the git `pre-push` hook path (already repo-owned, so it *does* run in fresh clones) that warns when `.claude/hooks/` is absent under a Claude-harness session. Effort S.
  Resolution: applied — slice (a): `doctor.sh` now runs `check-harness-provisioned.sh --quiet` as a warn-only preflight check (`[WARN] harness` unprovisioned / `[PASS] harness` wired; covered by `tests/bats/harness_provisioned_doctor.bats`). Slice (b)'s premise was wrong: `scripts/git-hooks/pre-push` is itself only wired via `core.hooksPath` BY `setup-harness.sh`, so no git hook runs in a fresh clone either — replaced with a doc note in `docs/harness/SETUP.md` § Check anytime stating that fact and pointing at the doctor probe.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P1 — AI assistant auto-context bodies are injected into the system prompt unsanitized (prompt-injection surface)
  Details: `ComposeSystemPrompt` (AiAssistantController.h) wraps each auto-context block in `<smatchet_context block="...">` tags and XML-escapes only the *attribute*; the *body* — ticket summaries, labels, audit-trail strings, visible grid rows, all attacker-influenceable via the tracker backend — is inserted verbatim. A malicious ticket summary can attempt closing-tag breakout or instruction injection into the model. The outbound-consent modal mitigates exfil *volume* (real byte counts) but shows sizes, not content, and does nothing against instruction injection. AGENTIC_INFRA_AUDIT.md finding B1.
  Concrete next action: (a) escape/neutralize `</smatchet_context` sequences in block bodies before assembly (pure helper, unit-testable in the existing tests/Core/AiAssistantSystemPrompt TU); (b) append one fixed line to the composed system prompt stating that content inside `smatchet_context` tags is data from the tracker, never instructions. Effort S.
  Resolution: applied — `NeutralizeContextBody` (AiXmlAttrEscape.h, pure) breaks `<smatchet_context`/`</smatchet_context` sequences in block bodies (`&lt;` on the leading `<`) at both assembly sites (`ComposeSystemPrompt` + `AiContextBuilder::AppendBlock`), and `ContextDataNotInstructionsLine()` adds the fixed data-not-instructions sentence after the context header; covered in tests/Core/AiAssistantSystemPrompt.test.cpp (breakout neutralized, benign unchanged, preamble iff blocks).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P2 — MCP `tools/call` has no rate limit; only SSE connection count is bounded
  Details: every MCP `tools/call` (JSON-RPC and the REST equivalent) dispatches into the command registry with bounded parsing and destructive gating, but no frequency bound — a buggy or hostile local client can hot-loop non-destructive commands (`tickets.search*`, `perf.dump`, ...) unthrottled. `CanAcceptSseConnection` bounds SSE streams (503 over-cap) but nothing bounds tool-call rate. Distinct from the archived "MCP registry dispatch un-gated after Authorize" entry (its destructive-confirm half shipped in PR #1246; its residual is capability *scoping*, not rate). AGENTIC_INFRA_AUDIT.md finding B3.
  Concrete next action: add a token-bucket at `DispatchRegistryToolsCall` in `Source/Plugins/Mcp/McpPlugin.cpp` (one chokepoint covers JSON-RPC + REST + legacy routes); return a structured `rate-limited` error envelope; make bucket size/refill configurable via `TrackerConfig` with a sane default; extract the decision to a pure helper for doctest coverage. Effort M.
  Resolution: applied — `ConsumeToolsCallToken` (McpRateLimitPure.h, pure token bucket, doctested in tests/Plugins/Mcp/McpRateLimit.test.cpp) gates both real entry points — REST `HandleToolsCall` and JSON-RPC `HandleJsonRpcToolsCall` (the JSON-RPC path does NOT funnel through `DispatchRegistryToolsCall`, so the gate sits one level up and covers every dispatch arm incl. run_lua/Lua tools/legacy) — sharing one bucket; deny returns the canonical HTTP-200 `rate-limited` envelope (REST) / JSON-RPC -32000 with retry-after; `TrackerConfig::McpToolsCallRateBurst`/`RateRefillPerSec` (default 20 burst / 5 per s, <=0 disables) persist as `mcp_tools_call_rate_*` and participate in `NeedsRestart`.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-06 · orchestrator (agentic-infra audit 2026-07) · [security] · P2 — debug `ai.dump-request` path re-implements AI client config/URL building and skips the production sanitizers
  Details: PARTIALLY LANDED (2026-07-08, backlog batch security-ai-mcp): the config half is unified — `SanitizeHeaderValue` + `BuildClientConfig` (key sanitizing, base-URL fallback chains, `EndpointPolicyForProvider` sanitize-with-consent gate, streaming timeout) moved from `AiAssistantController.cpp`'s anonymous namespace to the shared seam `Source/Core/src/AiRequestBuilder.cpp` (+ header), now consumed by the controller AND all three debug call sites (`ai.dump-request` / `ai.probe` / `ai.send-once`); the `BuildClientConfigForProvider` clone in `BuiltinCommands_Ai.cpp` is deleted, so the debug path no longer skips the sanitizers (doctested in tests/Core/AiRequestBuilder.test.cpp). REMAINING: the debug body/URL builders (`BuildAnthropicBody`/`BuildOpenAiBody`/`BuildOllamaNativeBody`/`ResolveEndpointUrl`/`StripOpenAiV1Suffix` in `BuiltinCommands_Ai.cpp`) still mirror the per-client `BuildChatBody`/`ResolveBaseUrl`/`JoinUrl` (anonymous namespaces in OpenAiClient/AnthropicClient/OllamaClient.cpp) instead of calling them — the residual drift surface. The archived 2026-05-17 entry records `ai.dump-request` already misreporting the wire once (fixed post-PR #184). AGENTIC_INFRA_AUDIT.md finding B4 / proposal P4.
  Concrete next action: expose the per-client body/URL builders (the `OllamaBuildRequestBodyJson` pattern already exists in OllamaClient.cpp) and make `ai.dump-request` call them, deleting the debug mirrors; then add doctest coverage asserting the debug dump equals the production wire for each provider.
  Status: applied (2026-07-11 — the remaining drift surface is closed: new `AiWireIntrospect.h` exposes `smatchet::ai::{OpenAi,Anthropic,OllamaNative}BuildChatBodyJson` + `...ResolveChatUrl`, each a thin wrapper over the SAME anonymous-namespace `BuildChatBody`/`ResolveBaseUrl`/`JoinUrl` the live client dispatch uses. `ai.dump-request` builds an `AiChatRequest` and calls them; the `BuildAnthropicBody`/`BuildOpenAiBody`/`BuildOllamaNativeBody`/`ResolveEndpointUrl`/`StripOpenAiV1Suffix` mirrors in BuiltinCommands_Ai.cpp are deleted. Because the dump now shares the production builder, it can no longer drift OR drop history (the mirrors only ever emitted a single user turn). Doctest `tests/Core/AiWireIntrospect.test.cpp` locks the per-provider wire shape incl. the full system+multi-turn body. Dual-target compiled.)
  Last-reviewed: 2026-07-11

- 2026-07-05 · orchestrator (mutation-testing pilot) · [tooling] · P2 — the mutation pilot built a small, reusable single-point-mutation harness that is a ready seed for roadmap Slice **F** (mutation-smoke / coverage-delta gate, `testing-surface-roadmap.md`)
  Details: the harness drives a JSON spec of `{file, search, replace}` mutants against `SmatchetTsanTests` — for each: assert `git` tree clean → apply exact single-point edit → `cmake --build --preset ninja-tsan-linux` (incremental) → run the exe → classify KILLED/SURVIVED/BUILD_FAIL → `git checkout` revert → re-assert clean. Cheap + deterministic on the doctest rig; catches assertion rot the coverage-delta gate structurally cannot see.
  Concrete next action (from the entry): promote the harness to `scripts/dev/mutation-smoke.sh` + a curated per-TU corpus, run it advisory-nightly over the dedicated-test TUs gating on a kill-rate floor, keep the equivalent-mutant exclusion list so the floor isn't gamed.
  Resolution: applied — Slice F's mutation-smoke half shipped across four phases (plan `docs/plans/mutation-smoke-gate.md`). Phase 1/2: `mutation-smoke.sh` + seed corpus + advisory nightly step in `tsan-linux-nightly.yml` + bats + local mirrors. Phase 3 (#1818, 2026-07-13): corpus expanded to 38 mutants (33 `killed` guards + 5 `equivalent`) covering all 20 dedicated-test TUs; found + fixed 1 genuine weak assertion (JIRAERR-02). Phase 4 (2026-07-16): after 3 consecutive clean advisory nightlies (07-14/15/16, each 33/33 killed @ 100% adjusted kill rate), `continue-on-error` removed → the gate now blocks the nightly on a sub-floor survivor. The equivalent-exclusion list (DT2/DT5/JQL-01/MAP-05/Labels-m3) is preserved in the corpus. Coverage-delta half remains out of scope (the plan's stated non-goal).
  Status: applied
  Last-reviewed: 2026-07-16

- 2026-07-05 · claude-code · [tooling] · P2 — lint: a non-"advisory"-named CI job must not carry job-level continue-on-error
  Details: the all-gates-blocking flip had THREE lanes drift out of sync between three coupled attributes — check name de-advisoried, step/job mask retained, required-context promoted (bucket-E, mobile-texture-guard, cpp-lint). The pre-ship code-review round caught them by hand (4 HIGH findings). A cheap mechanical gate would catch the class: scan `.github/workflows/*.yml` and FAIL if any job whose `name:` does NOT contain "advisory" (case-insensitive) sets job-level `continue-on-error: true`. Job-level masks green-wash the whole workflow run and are the anti-pattern the flip removed; step-level masks (the sanctioned per-step survivors: fuzz stochastic, bucket golden diff, bucket-E per-test, cpp-lint cppcheck) are exempt — the rule is job-level only. Cross-ref: shipped/all-gates-blocking.md.
  Resolution: applied — new gate `agents/scripts/core/test-workflow-job-mask.sh` (rule `gate-job-mask-non-advisory`): FAILs any workflow job whose name lacks "advisory" that sets job-level `continue-on-error` (literal `false` and step-level masks exempt; expression values count as masks); `--selftest` fixture + `tests/bats/workflow_job_mask.bats`; wired into doc-validation.yml beside the required-context-parity step.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code (nightly-monkey session) · [tooling] · P2 — `scripts/dev/coverage-delta-gate.sh` counts only `tests/{Core,Lua,Plugins,ui}/*.test.cpp` as a test-delta, so a PR that adds a whole NEW test directory (`tests/monkey/`) of real tests still red-walls the required `Test-delta gate`
  Details: the gate's `TEST_CHANGES` list was a fixed per-directory glob. PR #1637 added a genuine new seeded-fuzz harness under `tests/monkey/` paired with a behaviour-preserving Core extraction — but `tests/monkey/*` was invisible to `TEST_CHANGES` AND its `.cpp/.h` lines count as "real surface" in the `_classify_diff` exemption pre-check, so the gate reported `FAIL: Source/Core/ changes without test deltas` despite hundreds of added test lines. Distinct from the SIGPIPE-crash fix (#1593) and the platform-`#else`-arm exemption gap (#1021) — both are about the exemption classifier; this one is about the test-file recognition glob.
  Resolution: applied — option (a): `TEST_CHANGES` now recognizes any `tests/**/*.test.cpp` (with `tests/support/` + `tests/fixtures/` excluded as trivially-dismissable helper dirs), so a new harness dir earns gate credit via the `*.test.cpp` naming convention instead of a hand-synced directory allowlist; bats cases in `tests/bats/coverage_gate.bats` (new-dir `tests/monkey/*.test.cpp` delta → PASS; `tests/support/*.test.cpp` → no credit).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — doc-validation: flag a required_contexts addition that an ADR explicitly rejected
  Details: the all-gates-blocking flip's first draft silently added `Intent section` + `Plan-lock gate` to `branch_protection.required_contexts` — a route ADR-0022 and plan-lock-enforcement Q7 had EXPLICITLY REJECTED (the label hatches can't reach GitHub branch protection; `plan-lock-gate.yml` has no `labeled` re-trigger, so a red + override label = unmergeable). The code-review round caught it; a gate would catch the class. Cross-ref: shipped/all-gates-blocking.md § Deviations; docs/adr/0022-intent-gate-promotion.md.
  Resolution: applied — new `agents/scripts/core/test-required-context-adr-consistency.sh`: for each name ADDED to `branch_protection.required_contexts` vs `origin/develop`, greps `docs/adr/*.md` + `docs/plans/shipped/*.md` for the name inside a rejection window (±2 lines matching reject / "NOT a required" / "do not add" / "must not") and FAILs with the citation (base-ref-unreadable degrades to WARN+pass; CI uses fetch-depth 0); `--selftest` + `tests/bats/required_context_adr_consistency.bats`; wired into doc-validation.yml and `scripts/dev/test-docs.sh`; reproduces the ADR-0022 Intent-section/Plan-lock incident on a fixture.

- 2026-07-05 · orchestrator (docs-reconciliation session) · [process] · P2 — audit docs (`CPP_CODE_AUDIT.md`, `SECURITY_AUDIT.md`) were left presenting every finding as open long after the remediation PRs shipped; nothing flags an audit doc whose findings are fixed-in-code but still unmarked
  Details: `CPP_CODE_AUDIT.md` (2026-07-01) and `SECURITY_AUDIT.md` (2026-06-26) carried zero per-finding remediation status even though PR #1593/#1613 (code audit) and #1566 + follow-ups #1574/#1578/#1581/#1592/#1598 (security) had already fixed essentially every finding — a reader would conclude ~66 live defects were outstanding. The remediation plans (`cpp-code-audit-remediation.md`, `cpp-security-hardening.md`) tracked the fixes but the SOURCE audit docs they cite were never back-annotated, and the plans themselves sat in `docs/plans/active/` after all slices shipped. This entire session existed to reconcile that drift (added REMEDIATED banners + per-finding status tables to both audits, archived 5 shipped plans, refreshed the backlog/coverage docs). Root cause: a remediation PR updates the plan + code but not the originating audit doc, and no gate notices the divergence.
  Concrete next action: (1) encode "a remediation PR that closes findings from an audit doc updates that doc's per-finding status in the same PR" as a rule in `docs/agent-rules/process-rules.md`; and/or (2) add a lightweight advisory gate — for each root `*_AUDIT.md` whose companion remediation plan lives in `docs/plans/shipped/`, warn if the audit doc contains no `REMEDIATED`/✅ marker. Cheap heuristic, catches exactly this drift class before it accumulates. Cross-ref: this session's audit banners + `plan-archival-owed.sh` (the sibling nag that already covers the "shipped plan still in active/" half).
  Resolution: applied — rule encoded in docs/agent-rules/process-rules.md § Audit-doc status sync ('a remediation PR that closes findings from a root *_AUDIT.md updates that doc's per-finding status in the same PR'), plus the advisory backstop `agents/scripts/core/audit-doc-status-owed.sh` (--list/--nudge/--selftest, sibling of plan-archival-owed.sh; warns when a root *_AUDIT.md with a shipped companion remediation plan lacks a REMEDIATED/✅ marker), wired as a SessionStart nudge in the claude-code + codex harness templates.

- 2026-07-05 · claude-code · [tooling] · P3 — perf-compare delta table shows big % on 1-sample scopes without flagging them as below-floor noise
  Details: `scripts/dev/perf-compare.py`'s per-scenario delta table (surfaced in the `Perf PR-fast` job summary + PR comment) prints eye-catching relative deltas for scopes that have too few samples to be meaningful. On PR #1632's `ai-chat-history-render` run, `SmatchetUI::Draw` read `0.424 → 0.493 ms (+16.2 %)`, `SmatchetToolbarUi::Draw +56.9 %`, `SmatchetToastManager::Render +3575.0 %` — all with **`baseline calls = 1`**. The GATE correctly reports 0 regressions (the `min_baseline_calls = 10` floor + `mean_min_abs_delta_ms = 0.05` noise floor in `regression-policy.json` reject them), but the TABLE renders the raw percentages with no marker, so a human reading the PR sees "+3575 %" and reasonably suspects a real regression. This session had to hand-explain in the PR body why those aren't regressions — the presentation should carry that itself.
  Impact: not a gate bug (the gate is correct), but a **legibility** gap that produces false alarm + wasted triage on every low-sample scenario. The PR author / reviewer can't tell "this % is noise below the sample floor" from "this % is a real move" without cross-referencing the policy thresholds by hand.
  Concrete next action: in `perf-compare.py`'s table renderer, tag any row whose `baseline calls < min_baseline_calls` (or whose absolute delta < mean_min_abs_delta_ms) with an inline marker — e.g. append `· (noise: <N samples < floor)` or move such rows under a collapsed "below sample/noise floor — not gated" sub-section — so a reader distinguishes gated signal from sampling noise at a glance. Optionally sort gated-eligible rows first. Keep the raw numbers (transparency), just annotate.
  Cross-ref: PR #1632 Validation section (the hand-written noise explanation this would have made unnecessary); `docs/perf/regression-policy.json` (the floors).
  Resolution: applied — perf-compare.py's evaluate() now tags rows below the sample floor (`· (noise: N < M calls)`) or the absolute-delta noise floor (`· (noise: abs Δ ≤ X ms)`); emit_markdown sinks marked rows below the gated-eligible ones and appends a not-gated legend line. Raw numbers kept; gate behaviour unchanged (fixture-verified: floored rows exit 0, a real regression still exits 1).

- 2026-07-05 · orchestrator (docs-reconciliation session) · [tooling] · P2 — `scripts/dev/test-docs.sh` bills itself as the local mirror of `doc-validation.yml` but omits the `md_lint` (MD028 etc.) step the CI lane actually runs, so a doc author gets a green local mirror and then a red "Doc anchors + agent contract" CI lane on the same content
  Details: `test-docs.sh`'s own header reads "local mirror of the .github/workflows/doc-validation.yml gate", and it runs 14 checks (`test-doc-anchors`, `test-plan-index`, `test-plan-ref-integrity`, `test-markdown-links`, …) — but NOT `python3 agents/scripts/core/md_lint.py --all`, which the CI doc lane runs as its "md_lint — markdown style (MD028 etc.)" step. This session added a staleness blockquote to `backlog/MANUAL_TEST_QUEUE.md` that left a bare blank line between two adjacent blockquotes; `test-docs.sh` passed 14/14 locally, then CI failed `md_lint: MD028 blank line inside blockquote`, costing a diagnosis round-trip plus a fix commit. The mirror's entire value is "catch locally what CI catches"; a missing sub-check silently defeats that for the single most common markdown-authoring mistake.
  Concrete next action: add `md_lint` to the `CHECKS` array in `scripts/dev/test-docs.sh` (e.g. `"md_lint|python3 $CORE/md_lint.py --selftest && python3 $CORE/md_lint.py --all"`), mirroring how `doc-validation.yml` invokes it, so the local mirror is a true superset-or-equal of the CI doc lane. One-line addition, no new dependency (md_lint is pure Python already in-tree).
  Resolution: applied — `md_lint|python3 $CORE/md_lint.py --selftest && python3 $CORE/md_lint.py --all` added to the STEPS array in `scripts/dev/test-docs.sh`, positioned between test-plan-naming and test-portable-purity to mirror the doc-validation.yml step order; verified by running test-docs.sh locally (md_lint green).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — pre-push clang-format check is whole-file, not delta; pre-existing drift in a touched file blocks an unrelated change

  Details: `scripts/git-hooks/pre-push` step 3 runs `clang-format --dry-run --Werror
  "$ci_f"` over each **changed first-party C++ file as a whole**. The CI lint gate
  (`Windows + MSVC` clang-format step) is **delta-based** (flags only NEW violations
  vs origin/develop, grandfathering pre-existing drift), so the local hook is
  STRICTER than the gate it claims to mirror. Observed this session on the
  `perf-win-hunt` one-line change to `SmatchetAiAssistantUi.cpp`: my edit was
  clang-format-clean, but a PRE-EXISTING drift at line 1036 (an over-long
  `EnqueueAppendAndTrim` call from an earlier commit) tripped the whole-file
  `--Werror` and refused the push. The remedy (`clang-format -i` the file) then
  reformats a line I never touched, adding unrelated churn to the diff — or forces
  the `SMATCHET_SKIP_PRESHIP_GATE=1` override for a legitimately-clean change.

  Impact: low-frequency friction, but it (a) makes the hook disagree with CI (the
  parity the hook exists to provide — `docs/agent-rules/ci-local-parity.md`), and
  (b) nudges toward either scope-creep (reformatting untouched lines) or the
  sanctioned-but-noisy skip override.

  Concrete next action: make the pre-push clang-format check delta-aware to match
  the CI gate — e.g. `git clang-format --diff <merge-base>` (formats/checks only the
  changed hunks) instead of `clang-format --dry-run --Werror <whole-file>`. If a
  whole-file check is intentional (catch latent drift early), then it should
  *offer* to reformat only the changed hunks, and its message should say "whole-file
  (stricter than CI delta)" so the operator isn't surprised the hook rejects a
  CI-green change. Home: `scripts/git-hooks/pre-push` step 3.

  Resolution: applied — pre-push step 3 now runs `git clang-format --diff
  <merge-base> HEAD -- <changed first-party C++>` (delta: only changed hunks
  flag, matching the CI gate; rc=1 = violation, any other rc = infra →
  fail-open) and falls back to the whole-file `clang-format --dry-run --Werror`
  loop only when git-clang-format is absent, with the failure line then
  labelled "[whole-file — stricter than the CI delta]". Covered by
  `tests/bats/pre_push_format_delta.bats` (clean hunk atop pre-existing drift
  passes; bad new hunk still refuses).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · user-facing-text session (PRs #1614/#1615) · [infra] · P3 — remote-container builds: GitHub release tarballs 403 through the agent proxy; posix-core-check needs a manual curl clone + apt packages
  Details: in the Claude Code remote container the network policy allows `git clone` but returns 403 for GitHub release-asset and codeload tarball downloads; the `posix-core-check` configure fails at cpr's internal FetchContent of `curl-7.80.0.tar.xz`. `xorg-dev`/`libgl1-mesa-dev` are also not preinstalled (glfw's configure needs them even though it never builds in that preset) and need an `apt-get update` first. Validated workaround (2026-07-05 session): `git clone --depth 1 --branch curl-7_80_0 https://github.com/curl/curl.git .fetchcontent-src/curl-manual`, then `apt-get install -y xorg-dev libgl1-mesa-dev`, then `cmake --preset posix-core-check -DFETCHCONTENT_SOURCE_DIR_CURL=$PWD/.fetchcontent-src/curl-manual`. Proposal: fold the steps into a SessionStart hook or a `scripts/dev/remote-container-bootstrap.sh` so future remote sessions get a working posix-core-check lane without rediscovering the workaround.
  Resolution (2026-07-08): applied — `scripts/dev/remote-container-bootstrap.sh` wraps the workaround (idempotent: clone skipped when present, apt skipped when installed; `--no-configure` provisions only), referenced from `docs/agent-rules/build.md` § Remote-container posix-core-check bootstrap and `docs/harness/claude-code/setup.md` § Remote container; validated end-to-end in the target container (fresh configure green in ~80s; re-run skips both steps).

- 2026-07-05 · orchestrator (mutation-testing pilot) · [test] · P2 — the mutation pilot (`MUTATION_PILOT.md`) found 10 genuine weak assertions in the headless `SmatchetTsanTests` rig; 3 worst fixed in the pilot PR, **7 residual survivors** remain unasserted (each a plausible single-point bug the current suite would ship uncaught)
  Details: 68 mutants over 12 TUs → 52 killed / 16 survived (5 equivalent, 1 out-of-oracle, 10 real weak assertions). The 7 residual (all reproduced + diffed in `MUTATION_PILOT.md` § "Every surviving mutant"):
  - `TrackerGridFieldDisplayPure.cpp` **GR5** — `if (s.MaxResults > 0)` → `>= 0`: "Page size (maxResults):" tooltip line emitted at 0, no subcase asserts it.
  - `TrackerGridFieldDisplayPure.cpp` **GR6** — `if (s.Total > 0 && s.WorklogsOnPage > 0)` → `||`: "This page: a–b of N" tooltip branch unexercised when exactly one operand is 0.
  - `PlaneQuerySuggestEnginePure.cpp` **PLANE-03** — `if (raw.empty())` guard in `tryAdd` neutralised: an empty catalog option value would emit an empty suggestion; no field carries an empty option value.
  - `JqlSuggestEnginePure.cpp` **JQL-03** — `if (++added >= kMaxUsers)` → `>`: the 50-user suggestion cap boundary (50 vs 51 emitted) is never tested.
  - `LinearQueryFromJql.cpp` **JQL-05** — `if (s.size() >= 2 ...)` → `> 2`: 2-char quoted operand (`""`/`''`) unquote edge unasserted.
  - `MergeWatchNotifyPure.cpp` **m3** — `if (out.size() > kMaxMessageBytes)` → `>=`: exact-at-cap truncation of the localhost-listener payload (SECURITY_AUDIT Tier-1 #6) — test uses 600 B, never exactly `kMaxMessageBytes`.
  - `LinearClientHelpers.cpp` **m5** — `negative = (s[0] == '-')` → `false`: `ParseLongOr` negative-magnitude path (incl. `LONG_MIN` reconstruction) unasserted; `ParseLinearRateLimitHeaders` only tested with positive values.
  Concrete next action: add the pinning assertions (each is a 1–3 line addition to the existing suite, template proven by the 3 fixed in the pilot PR): GR5/GR6 assert the tooltip strings on `maxResults==0` / `total==0,page>0` shapes; PLANE-03 feeds an empty-value option and asserts no empty suggestion; JQL-03 builds 51 matching users and asserts the cap; LinearQueryFromJql JQL-05 asserts `""` round-trips; MergeWatch m3 asserts an exactly-`kMaxMessageBytes` message is not truncated; LinearClientHelpers m5 asserts a negative `x-complexity` header parses to its signed value. NB: 5 mutants that survived are EQUIVALENT (DT2, DT5, JQL-01-notin, MAP-05-reserve, Labels-m3 — documented, do not "fix"). Cross-ref: `MUTATION_PILOT.md`, plan `docs/plans/mutation-testing-pilot.md`, roadmap Slice F (`testing-surface-roadmap.md`).
  Resolution: applied — all 7 pinning assertions added to the existing pure doctest TUs (GR5/GR6 in TrackerGridFieldDisplayPure.test.cpp, PLANE-03 in PlaneQuerySuggestEnginePure.test.cpp, JQL-03 in JqlSuggestEnginePure.test.cpp, JQL-05 in LinearQueryFromJql.test.cpp, m3 in MergeWatchNotifyPure.test.cpp, m5 in LinearClientHelpers.test.cpp). Each mutant re-applied locally against the Linux `ninja-tsan-linux` rig: all 7 now KILLED; the 5 documented equivalent mutants were left alone.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-05 · orchestrator (mutation-testing pilot) · [infra] · P2 — the `ninja-tsan-linux` preset compiles but **fails to link** on a fresh container: the Clang TSan runtime archive (`libclang_rt.tsan-x86_64.a`) is absent from the image, so `SmatchetTsanTests` cannot be built or run without a manual `apt-get install libclang-rt-18-dev` first
  Details: on this Linux image `clang-18` is present but `/usr/lib/llvm-18/lib/clang/18/lib/linux/` (the compiler-rt sanitizer archives) does not exist until `libclang-rt-18-dev` is installed. All 93 TUs of `SmatchetTsanTests` compiled cleanly; only the final link failed (`ld.lld: cannot open .../libclang_rt.tsan-x86_64.a`). This is the ONLY assertion-based test executable that builds+runs headless on Linux (the primary doctest/UI rigs need MSVC ABI or ImGui/GLFW/X11/GL), so any Linux/web session doing test or mutation work hits this wall first. The nightly `tsan-linux-nightly.yml` CI runner presumably has the package pre-installed, masking the gap for local/container sessions.
  Concrete next action: add `libclang-rt-18-dev` (or the toolchain-matched `libclang-rt-$LLVM_VERSION-dev`) to the SessionStart provisioning / devcontainer setup so `ninja-tsan-linux` links out-of-the-box; alternatively document the one-liner in `docs/agent-rules/build.md` next to the tsan preset. Cheap, unblocks the entire headless-Linux test surface. Cross-ref: `MUTATION_PILOT.md` § Phase 0 footnote 1; `CMakePresets.json` `ninja-tsan-linux`.
  Resolution: applied — docs/agent-rules/build.md gained a "TSan on Linux" section with the `libclang-rt-18-dev` one-liner next to the preset docs, verified end-to-end in the exhibiting container (install → configure → build → link → suite green). The remote-container bootstrap-script fold is deliberately left to the `remote-container-fetchcontent-403` entry, whose PR creates `scripts/dev/remote-container-bootstrap.sh`.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-05 · claude-code · [tooling] · P2 — plan-lock records the CURRENT branch; claiming from the wrong tree self-collides with your own push

  Details: `agents/scripts/core/lock-claim.sh <slug> <write-set>` stamps the lock's
  owner branch as **whatever branch the invoking tree is on** (`git rev-parse
  --abbrev-ref HEAD`). This session claimed `refs/locks/perf-win-hunt` from the MAIN
  repo tree (`/c/Development/Smatchet`, on `develop`) while the actual work + the
  push happened in a WORKTREE on `perf/win-hunt`. Result: the lock recorded
  `branch=develop`, and the pre-push plan-lock guard then rejected the
  `perf/win-hunt` push as a **collision with a DIFFERENT branch's write-set** — the
  agent colliding with its own lock. Recovery was a delete-ref + re-claim from the
  worktree (so `branch=perf/win-hunt`), plus a wasted push cycle.

  The confusing part: the lock and the branch are BOTH the operator's, so "plan-lock
  collision — overlaps the write set owned by a DIFFERENT branch" reads as if a
  second session is contending, when really it's a self-inflicted branch mismatch.

  Concrete next action (pick one):
  1. **Warn on tree/branch mismatch:** in `lock-claim.sh`, if the current branch is
     the repo's default/integration branch (`develop`/`main`) — an unlikely branch
     to hold a feature plan-lock — emit a loud "claiming lock owner=<branch>; you
     usually claim from the feature worktree, not the integration tree" note before
     the push. Cheapest, non-breaking.
  2. **Let the branch be explicit:** accept an optional `--branch <name>` (or
     `LOCK_CLAIM_BRANCH` env) so the caller pins the intended owner regardless of
     which tree runs the script — mirrors the worktree-per-session model.
  3. **Doc the gotcha** in `docs/perforce/AGENT_FLOWS.md` / the plan-lock section:
     "claim the lock from the SAME worktree that will push, so owner == pushing
     branch." (Do this regardless of 1/2.)

  Cross-ref: session PR #1632 (perf-win-hunt) — the lock claimed on `develop` blocked
  the `perf/win-hunt` push until released + re-claimed from the worktree.
  Resolution: applied — satisfied by the explicit `LOCK_BRANCH` env override (`lock-claim.sh` header + `branch=` resolution, the entry's option 2) plus the `docs/agent-rules/ship-loops.md:140-145` mandate to pass `LOCK_BRANCH` explicitly from the worktree HEAD with the detached-HEAD skip (option 3); option 1's loud integration-branch warning added to `lock-claim.sh` in this archival PR.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — required-context "teeth" check: can this required context ever red a PR?

  Details: `test-required-context-parity.sh` verifies each
  `branch_protection.required_contexts` name matches a workflow job `name:`
  (byte-exact) — but not whether that job can EVER fail a PR. The all-gates-blocking
  review found a required context (`C++ lint`) that structurally could not fail
  (job-level mask + `cppcheck --error-exitcode=0`) and two (`High-integrity
  baseline/narrowing`) that always skip on PRs (`if: github.event_name == 'push'`)
  — required checks implying protection that doesn't exist. Add a heuristic warn:
  a required context whose hosting job is `if:`-gated to exclude `pull_request`,
  OR whose every failing path is masked, is a NO-OP gate. Emit WARN (not FAIL —
  a skip-on-PR job is legitimately vacuously-satisfied for merge-queue readiness),
  naming the vacuous contexts so a human confirms intent. Home: extend
  `test-required-context-parity.sh`. Cross-ref: shipped/all-gates-blocking.md § Deviations.
  Resolution: applied — the pr_triggered teeth shipped in `agents/scripts/core/test-required-context-parity.sh` (:93-102; selftest :190-191 asserts a push-only job hosting a required context FAILs — stronger than the proposed WARN); the residual every-failing-path-is-masked heuristic stays tracked by the sibling `tooling/2026-07-05-gate-lane-no-job-level-continue-on-error.md` entry (single tracker, no dual bookkeeping).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · orchestrator (recurring-findings gate campaign #1605 ship session) · [tooling] · P2 — `native-automerge-bypasses-merge-snapshot-ledger`: a PR merged via GitHub's native auto-merge appended no line to `docs/self-improvement/merge-snapshots.jsonl`, so the ADR-0017 lossless merge-time capture had a hole for exactly the merge path a session without the `gh` CLI ends up arming.
  Details: ship-loops.md § merge-snapshot mandated the append for three actors (in-session orchestrator REST merge, `git-janitor`, `merge-watcher handle_pass()`), but a session that ARMS GitHub-native auto-merge is none of them — the merge fires server-side, possibly after the session goes idle, and nothing writes the row. Hit live on #1605/#1608 (cloud session; no `gh`, so `safe-merge.sh` could not run; full gate set hand-verified before arming). Sweeper-workflow alternatives were evaluated and rejected: a GITHUB_TOKEN workflow can neither push develop (required-status-check protection, non-bypass actor) nor open harvest PRs that trigger the required contexts (GITHUB_TOKEN-created PRs spawn no workflow runs), and a scheduled retro-composer would write confidently-wrong rows from an already-rewritten rollup — a stale line is worse than a hole, since `postmortem-owed.sh` reads the ledger BEFORE the live fallback.
  Resolution: applied — ship-loops.md § merge-snapshot gained the **fourth writer**: the session that armed the auto-merge appends the row on receiving the merged notification (PR-activity webhook / check-in), fetching `mergeCommit`/`headSha`/labels via MCP when `gh` is absent, calling the same idempotent helper with `mergeActor=orchestrator-automerge` + `SNAPSHOT_MERGED_AT=<mergedAt>`, and landing it in its next develop-bound commit. The #1605 + #1608 rows were seeded through exactly that path in the same PR. Residual (accepted, documented in the mandate): a session that dies before the merge event, or with no further develop-bound commit, leaves the hole to ADR-0017's live fallback — best-effort, never blindness; no retro-composition.
  Status: applied
  Last-reviewed: 2026-07-05

- 2026-07-05 · orchestrator (mutation-testing pilot) · [tooling] · P2 — the mutation pilot built a small, reusable single-point-mutation harness that is a ready seed for roadmap Slice **F** (mutation-smoke / coverage-delta gate, `testing-surface-roadmap.md`)
  Details: the harness drives a JSON spec of `{file, search, replace}` mutants against `SmatchetTsanTests` — for each: assert `git` tree clean → apply exact single-point edit → `cmake --build --preset ninja-tsan-linux` (incremental) → run the exe → classify KILLED/SURVIVED/BUILD_FAIL → `git checkout` revert → re-assert clean. Cheap + deterministic on the doctest rig; catches assertion rot the coverage-delta gate structurally cannot see.
  Concrete next action (from the entry): promote the harness to `scripts/dev/mutation-smoke.sh` + a curated per-TU corpus, run it advisory-nightly over the dedicated-test TUs gating on a kill-rate floor, keep the equivalent-mutant exclusion list so the floor isn't gamed.
  Resolution: applied — Slice F's mutation-smoke half shipped across four phases (plan `docs/plans/mutation-smoke-gate.md`). Phase 1/2: `mutation-smoke.sh` + seed corpus + advisory nightly step in `tsan-linux-nightly.yml` + bats + local mirrors. Phase 3 (#1818, 2026-07-13): corpus expanded to 38 mutants (33 `killed` guards + 5 `equivalent`) covering all 20 dedicated-test TUs; found + fixed 1 genuine weak assertion (JIRAERR-02). Phase 4 (2026-07-16): after 3 consecutive clean advisory nightlies (07-14/15/16, each 33/33 killed @ 100% adjusted kill rate), `continue-on-error` removed → the gate now blocks the nightly on a sub-floor survivor. The equivalent-exclusion list (DT2/DT5/JQL-01/MAP-05/Labels-m3) is preserved in the corpus. Coverage-delta half remains out of scope (the plan's stated non-goal).
  Status: applied
  Last-reviewed: 2026-07-16

- 2026-07-05 · claude-code · [tooling] · P2 — lint: a non-"advisory"-named CI job must not carry job-level continue-on-error
  Details: the all-gates-blocking flip had THREE lanes drift out of sync between three coupled attributes — check name de-advisoried, step/job mask retained, required-context promoted (bucket-E, mobile-texture-guard, cpp-lint). The pre-ship code-review round caught them by hand (4 HIGH findings). A cheap mechanical gate would catch the class: scan `.github/workflows/*.yml` and FAIL if any job whose `name:` does NOT contain "advisory" (case-insensitive) sets job-level `continue-on-error: true`. Job-level masks green-wash the whole workflow run and are the anti-pattern the flip removed; step-level masks (the sanctioned per-step survivors: fuzz stochastic, bucket golden diff, bucket-E per-test, cpp-lint cppcheck) are exempt — the rule is job-level only. Cross-ref: shipped/all-gates-blocking.md.
  Resolution: applied — new gate `agents/scripts/core/test-workflow-job-mask.sh` (rule `gate-job-mask-non-advisory`): FAILs any workflow job whose name lacks "advisory" that sets job-level `continue-on-error` (literal `false` and step-level masks exempt; expression values count as masks); `--selftest` fixture + `tests/bats/workflow_job_mask.bats`; wired into doc-validation.yml beside the required-context-parity step.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code (nightly-monkey session) · [tooling] · P2 — `scripts/dev/coverage-delta-gate.sh` counts only `tests/{Core,Lua,Plugins,ui}/*.test.cpp` as a test-delta, so a PR that adds a whole NEW test directory (`tests/monkey/`) of real tests still red-walls the required `Test-delta gate`
  Details: the gate's `TEST_CHANGES` list was a fixed per-directory glob. PR #1637 added a genuine new seeded-fuzz harness under `tests/monkey/` paired with a behaviour-preserving Core extraction — but `tests/monkey/*` was invisible to `TEST_CHANGES` AND its `.cpp/.h` lines count as "real surface" in the `_classify_diff` exemption pre-check, so the gate reported `FAIL: Source/Core/ changes without test deltas` despite hundreds of added test lines. Distinct from the SIGPIPE-crash fix (#1593) and the platform-`#else`-arm exemption gap (#1021) — both are about the exemption classifier; this one is about the test-file recognition glob.
  Resolution: applied — option (a): `TEST_CHANGES` now recognizes any `tests/**/*.test.cpp` (with `tests/support/` + `tests/fixtures/` excluded as trivially-dismissable helper dirs), so a new harness dir earns gate credit via the `*.test.cpp` naming convention instead of a hand-synced directory allowlist; bats cases in `tests/bats/coverage_gate.bats` (new-dir `tests/monkey/*.test.cpp` delta → PASS; `tests/support/*.test.cpp` → no credit).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — doc-validation: flag a required_contexts addition that an ADR explicitly rejected
  Details: the all-gates-blocking flip's first draft silently added `Intent section` + `Plan-lock gate` to `branch_protection.required_contexts` — a route ADR-0022 and plan-lock-enforcement Q7 had EXPLICITLY REJECTED (the label hatches can't reach GitHub branch protection; `plan-lock-gate.yml` has no `labeled` re-trigger, so a red + override label = unmergeable). The code-review round caught it; a gate would catch the class. Cross-ref: shipped/all-gates-blocking.md § Deviations; docs/adr/0022-intent-gate-promotion.md.
  Resolution: applied — new `agents/scripts/core/test-required-context-adr-consistency.sh`: for each name ADDED to `branch_protection.required_contexts` vs `origin/develop`, greps `docs/adr/*.md` + `docs/plans/shipped/*.md` for the name inside a rejection window (±2 lines matching reject / "NOT a required" / "do not add" / "must not") and FAILs with the citation (base-ref-unreadable degrades to WARN+pass; CI uses fetch-depth 0); `--selftest` + `tests/bats/required_context_adr_consistency.bats`; wired into doc-validation.yml and `scripts/dev/test-docs.sh`; reproduces the ADR-0022 Intent-section/Plan-lock incident on a fixture.

- 2026-07-05 · orchestrator (docs-reconciliation session) · [process] · P2 — audit docs (`CPP_CODE_AUDIT.md`, `SECURITY_AUDIT.md`) were left presenting every finding as open long after the remediation PRs shipped; nothing flags an audit doc whose findings are fixed-in-code but still unmarked
  Details: `CPP_CODE_AUDIT.md` (2026-07-01) and `SECURITY_AUDIT.md` (2026-06-26) carried zero per-finding remediation status even though PR #1593/#1613 (code audit) and #1566 + follow-ups #1574/#1578/#1581/#1592/#1598 (security) had already fixed essentially every finding — a reader would conclude ~66 live defects were outstanding. The remediation plans (`cpp-code-audit-remediation.md`, `cpp-security-hardening.md`) tracked the fixes but the SOURCE audit docs they cite were never back-annotated, and the plans themselves sat in `docs/plans/active/` after all slices shipped. This entire session existed to reconcile that drift (added REMEDIATED banners + per-finding status tables to both audits, archived 5 shipped plans, refreshed the backlog/coverage docs). Root cause: a remediation PR updates the plan + code but not the originating audit doc, and no gate notices the divergence.
  Concrete next action: (1) encode "a remediation PR that closes findings from an audit doc updates that doc's per-finding status in the same PR" as a rule in `docs/agent-rules/process-rules.md`; and/or (2) add a lightweight advisory gate — for each root `*_AUDIT.md` whose companion remediation plan lives in `docs/plans/shipped/`, warn if the audit doc contains no `REMEDIATED`/✅ marker. Cheap heuristic, catches exactly this drift class before it accumulates. Cross-ref: this session's audit banners + `plan-archival-owed.sh` (the sibling nag that already covers the "shipped plan still in active/" half).
  Resolution: applied — rule encoded in docs/agent-rules/process-rules.md § Audit-doc status sync ('a remediation PR that closes findings from a root *_AUDIT.md updates that doc's per-finding status in the same PR'), plus the advisory backstop `agents/scripts/core/audit-doc-status-owed.sh` (--list/--nudge/--selftest, sibling of plan-archival-owed.sh; warns when a root *_AUDIT.md with a shipped companion remediation plan lacks a REMEDIATED/✅ marker), wired as a SessionStart nudge in the claude-code + codex harness templates.

- 2026-07-05 · claude-code · [tooling] · P3 — perf-compare delta table shows big % on 1-sample scopes without flagging them as below-floor noise
  Details: `scripts/dev/perf-compare.py`'s per-scenario delta table (surfaced in the `Perf PR-fast` job summary + PR comment) prints eye-catching relative deltas for scopes that have too few samples to be meaningful. On PR #1632's `ai-chat-history-render` run, `SmatchetUI::Draw` read `0.424 → 0.493 ms (+16.2 %)`, `SmatchetToolbarUi::Draw +56.9 %`, `SmatchetToastManager::Render +3575.0 %` — all with **`baseline calls = 1`**. The GATE correctly reports 0 regressions (the `min_baseline_calls = 10` floor + `mean_min_abs_delta_ms = 0.05` noise floor in `regression-policy.json` reject them), but the TABLE renders the raw percentages with no marker, so a human reading the PR sees "+3575 %" and reasonably suspects a real regression. This session had to hand-explain in the PR body why those aren't regressions — the presentation should carry that itself.
  Impact: not a gate bug (the gate is correct), but a **legibility** gap that produces false alarm + wasted triage on every low-sample scenario. The PR author / reviewer can't tell "this % is noise below the sample floor" from "this % is a real move" without cross-referencing the policy thresholds by hand.
  Concrete next action: in `perf-compare.py`'s table renderer, tag any row whose `baseline calls < min_baseline_calls` (or whose absolute delta < mean_min_abs_delta_ms) with an inline marker — e.g. append `· (noise: <N samples < floor)` or move such rows under a collapsed "below sample/noise floor — not gated" sub-section — so a reader distinguishes gated signal from sampling noise at a glance. Optionally sort gated-eligible rows first. Keep the raw numbers (transparency), just annotate.
  Cross-ref: PR #1632 Validation section (the hand-written noise explanation this would have made unnecessary); `docs/perf/regression-policy.json` (the floors).
  Resolution: applied — perf-compare.py's evaluate() now tags rows below the sample floor (`· (noise: N < M calls)`) or the absolute-delta noise floor (`· (noise: abs Δ ≤ X ms)`); emit_markdown sinks marked rows below the gated-eligible ones and appends a not-gated legend line. Raw numbers kept; gate behaviour unchanged (fixture-verified: floored rows exit 0, a real regression still exits 1).

- 2026-07-05 · orchestrator (docs-reconciliation session) · [tooling] · P2 — `scripts/dev/test-docs.sh` bills itself as the local mirror of `doc-validation.yml` but omits the `md_lint` (MD028 etc.) step the CI lane actually runs, so a doc author gets a green local mirror and then a red "Doc anchors + agent contract" CI lane on the same content
  Details: `test-docs.sh`'s own header reads "local mirror of the .github/workflows/doc-validation.yml gate", and it runs 14 checks (`test-doc-anchors`, `test-plan-index`, `test-plan-ref-integrity`, `test-markdown-links`, …) — but NOT `python3 agents/scripts/core/md_lint.py --all`, which the CI doc lane runs as its "md_lint — markdown style (MD028 etc.)" step. This session added a staleness blockquote to `backlog/MANUAL_TEST_QUEUE.md` that left a bare blank line between two adjacent blockquotes; `test-docs.sh` passed 14/14 locally, then CI failed `md_lint: MD028 blank line inside blockquote`, costing a diagnosis round-trip plus a fix commit. The mirror's entire value is "catch locally what CI catches"; a missing sub-check silently defeats that for the single most common markdown-authoring mistake.
  Concrete next action: add `md_lint` to the `CHECKS` array in `scripts/dev/test-docs.sh` (e.g. `"md_lint|python3 $CORE/md_lint.py --selftest && python3 $CORE/md_lint.py --all"`), mirroring how `doc-validation.yml` invokes it, so the local mirror is a true superset-or-equal of the CI doc lane. One-line addition, no new dependency (md_lint is pure Python already in-tree).
  Resolution: applied — `md_lint|python3 $CORE/md_lint.py --selftest && python3 $CORE/md_lint.py --all` added to the STEPS array in `scripts/dev/test-docs.sh`, positioned between test-plan-naming and test-portable-purity to mirror the doc-validation.yml step order; verified by running test-docs.sh locally (md_lint green).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — pre-push clang-format check is whole-file, not delta; pre-existing drift in a touched file blocks an unrelated change

  Details: `scripts/git-hooks/pre-push` step 3 runs `clang-format --dry-run --Werror
  "$ci_f"` over each **changed first-party C++ file as a whole**. The CI lint gate
  (`Windows + MSVC` clang-format step) is **delta-based** (flags only NEW violations
  vs origin/develop, grandfathering pre-existing drift), so the local hook is
  STRICTER than the gate it claims to mirror. Observed this session on the
  `perf-win-hunt` one-line change to `SmatchetAiAssistantUi.cpp`: my edit was
  clang-format-clean, but a PRE-EXISTING drift at line 1036 (an over-long
  `EnqueueAppendAndTrim` call from an earlier commit) tripped the whole-file
  `--Werror` and refused the push. The remedy (`clang-format -i` the file) then
  reformats a line I never touched, adding unrelated churn to the diff — or forces
  the `SMATCHET_SKIP_PRESHIP_GATE=1` override for a legitimately-clean change.

  Impact: low-frequency friction, but it (a) makes the hook disagree with CI (the
  parity the hook exists to provide — `docs/agent-rules/ci-local-parity.md`), and
  (b) nudges toward either scope-creep (reformatting untouched lines) or the
  sanctioned-but-noisy skip override.

  Concrete next action: make the pre-push clang-format check delta-aware to match
  the CI gate — e.g. `git clang-format --diff <merge-base>` (formats/checks only the
  changed hunks) instead of `clang-format --dry-run --Werror <whole-file>`. If a
  whole-file check is intentional (catch latent drift early), then it should
  *offer* to reformat only the changed hunks, and its message should say "whole-file
  (stricter than CI delta)" so the operator isn't surprised the hook rejects a
  CI-green change. Home: `scripts/git-hooks/pre-push` step 3.

  Resolution: applied — pre-push step 3 now runs `git clang-format --diff
  <merge-base> HEAD -- <changed first-party C++>` (delta: only changed hunks
  flag, matching the CI gate; rc=1 = violation, any other rc = infra →
  fail-open) and falls back to the whole-file `clang-format --dry-run --Werror`
  loop only when git-clang-format is absent, with the failure line then
  labelled "[whole-file — stricter than the CI delta]". Covered by
  `tests/bats/pre_push_format_delta.bats` (clean hunk atop pre-existing drift
  passes; bad new hunk still refuses).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · user-facing-text session (PRs #1614/#1615) · [infra] · P3 — remote-container builds: GitHub release tarballs 403 through the agent proxy; posix-core-check needs a manual curl clone + apt packages
  Details: in the Claude Code remote container the network policy allows `git clone` but returns 403 for GitHub release-asset and codeload tarball downloads; the `posix-core-check` configure fails at cpr's internal FetchContent of `curl-7.80.0.tar.xz`. `xorg-dev`/`libgl1-mesa-dev` are also not preinstalled (glfw's configure needs them even though it never builds in that preset) and need an `apt-get update` first. Validated workaround (2026-07-05 session): `git clone --depth 1 --branch curl-7_80_0 https://github.com/curl/curl.git .fetchcontent-src/curl-manual`, then `apt-get install -y xorg-dev libgl1-mesa-dev`, then `cmake --preset posix-core-check -DFETCHCONTENT_SOURCE_DIR_CURL=$PWD/.fetchcontent-src/curl-manual`. Proposal: fold the steps into a SessionStart hook or a `scripts/dev/remote-container-bootstrap.sh` so future remote sessions get a working posix-core-check lane without rediscovering the workaround.
  Resolution (2026-07-08): applied — `scripts/dev/remote-container-bootstrap.sh` wraps the workaround (idempotent: clone skipped when present, apt skipped when installed; `--no-configure` provisions only), referenced from `docs/agent-rules/build.md` § Remote-container posix-core-check bootstrap and `docs/harness/claude-code/setup.md` § Remote container; validated end-to-end in the target container (fresh configure green in ~80s; re-run skips both steps).

- 2026-07-05 · orchestrator (mutation-testing pilot) · [test] · P2 — the mutation pilot (`MUTATION_PILOT.md`) found 10 genuine weak assertions in the headless `SmatchetTsanTests` rig; 3 worst fixed in the pilot PR, **7 residual survivors** remain unasserted (each a plausible single-point bug the current suite would ship uncaught)
  Details: 68 mutants over 12 TUs → 52 killed / 16 survived (5 equivalent, 1 out-of-oracle, 10 real weak assertions). The 7 residual (all reproduced + diffed in `MUTATION_PILOT.md` § "Every surviving mutant"):
  - `TrackerGridFieldDisplayPure.cpp` **GR5** — `if (s.MaxResults > 0)` → `>= 0`: "Page size (maxResults):" tooltip line emitted at 0, no subcase asserts it.
  - `TrackerGridFieldDisplayPure.cpp` **GR6** — `if (s.Total > 0 && s.WorklogsOnPage > 0)` → `||`: "This page: a–b of N" tooltip branch unexercised when exactly one operand is 0.
  - `PlaneQuerySuggestEnginePure.cpp` **PLANE-03** — `if (raw.empty())` guard in `tryAdd` neutralised: an empty catalog option value would emit an empty suggestion; no field carries an empty option value.
  - `JqlSuggestEnginePure.cpp` **JQL-03** — `if (++added >= kMaxUsers)` → `>`: the 50-user suggestion cap boundary (50 vs 51 emitted) is never tested.
  - `LinearQueryFromJql.cpp` **JQL-05** — `if (s.size() >= 2 ...)` → `> 2`: 2-char quoted operand (`""`/`''`) unquote edge unasserted.
  - `MergeWatchNotifyPure.cpp` **m3** — `if (out.size() > kMaxMessageBytes)` → `>=`: exact-at-cap truncation of the localhost-listener payload (SECURITY_AUDIT Tier-1 #6) — test uses 600 B, never exactly `kMaxMessageBytes`.
  - `LinearClientHelpers.cpp` **m5** — `negative = (s[0] == '-')` → `false`: `ParseLongOr` negative-magnitude path (incl. `LONG_MIN` reconstruction) unasserted; `ParseLinearRateLimitHeaders` only tested with positive values.
  Concrete next action: add the pinning assertions (each is a 1–3 line addition to the existing suite, template proven by the 3 fixed in the pilot PR): GR5/GR6 assert the tooltip strings on `maxResults==0` / `total==0,page>0` shapes; PLANE-03 feeds an empty-value option and asserts no empty suggestion; JQL-03 builds 51 matching users and asserts the cap; LinearQueryFromJql JQL-05 asserts `""` round-trips; MergeWatch m3 asserts an exactly-`kMaxMessageBytes` message is not truncated; LinearClientHelpers m5 asserts a negative `x-complexity` header parses to its signed value. NB: 5 mutants that survived are EQUIVALENT (DT2, DT5, JQL-01-notin, MAP-05-reserve, Labels-m3 — documented, do not "fix"). Cross-ref: `MUTATION_PILOT.md`, plan `docs/plans/mutation-testing-pilot.md`, roadmap Slice F (`testing-surface-roadmap.md`).
  Resolution: applied — all 7 pinning assertions added to the existing pure doctest TUs (GR5/GR6 in TrackerGridFieldDisplayPure.test.cpp, PLANE-03 in PlaneQuerySuggestEnginePure.test.cpp, JQL-03 in JqlSuggestEnginePure.test.cpp, JQL-05 in LinearQueryFromJql.test.cpp, m3 in MergeWatchNotifyPure.test.cpp, m5 in LinearClientHelpers.test.cpp). Each mutant re-applied locally against the Linux `ninja-tsan-linux` rig: all 7 now KILLED; the 5 documented equivalent mutants were left alone.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-05 · orchestrator (mutation-testing pilot) · [infra] · P2 — the `ninja-tsan-linux` preset compiles but **fails to link** on a fresh container: the Clang TSan runtime archive (`libclang_rt.tsan-x86_64.a`) is absent from the image, so `SmatchetTsanTests` cannot be built or run without a manual `apt-get install libclang-rt-18-dev` first
  Details: on this Linux image `clang-18` is present but `/usr/lib/llvm-18/lib/clang/18/lib/linux/` (the compiler-rt sanitizer archives) does not exist until `libclang-rt-18-dev` is installed. All 93 TUs of `SmatchetTsanTests` compiled cleanly; only the final link failed (`ld.lld: cannot open .../libclang_rt.tsan-x86_64.a`). This is the ONLY assertion-based test executable that builds+runs headless on Linux (the primary doctest/UI rigs need MSVC ABI or ImGui/GLFW/X11/GL), so any Linux/web session doing test or mutation work hits this wall first. The nightly `tsan-linux-nightly.yml` CI runner presumably has the package pre-installed, masking the gap for local/container sessions.
  Concrete next action: add `libclang-rt-18-dev` (or the toolchain-matched `libclang-rt-$LLVM_VERSION-dev`) to the SessionStart provisioning / devcontainer setup so `ninja-tsan-linux` links out-of-the-box; alternatively document the one-liner in `docs/agent-rules/build.md` next to the tsan preset. Cheap, unblocks the entire headless-Linux test surface. Cross-ref: `MUTATION_PILOT.md` § Phase 0 footnote 1; `CMakePresets.json` `ninja-tsan-linux`.
  Resolution: applied — docs/agent-rules/build.md gained a "TSan on Linux" section with the `libclang-rt-18-dev` one-liner next to the preset docs, verified end-to-end in the exhibiting container (install → configure → build → link → suite green). The remote-container bootstrap-script fold is deliberately left to the `remote-container-fetchcontent-403` entry, whose PR creates `scripts/dev/remote-container-bootstrap.sh`.
  Status: applied
  Last-reviewed: 2026-07-09

- 2026-07-05 · claude-code · [tooling] · P2 — plan-lock records the CURRENT branch; claiming from the wrong tree self-collides with your own push

  Details: `agents/scripts/core/lock-claim.sh <slug> <write-set>` stamps the lock's
  owner branch as **whatever branch the invoking tree is on** (`git rev-parse
  --abbrev-ref HEAD`). This session claimed `refs/locks/perf-win-hunt` from the MAIN
  repo tree (`/c/Development/Smatchet`, on `develop`) while the actual work + the
  push happened in a WORKTREE on `perf/win-hunt`. Result: the lock recorded
  `branch=develop`, and the pre-push plan-lock guard then rejected the
  `perf/win-hunt` push as a **collision with a DIFFERENT branch's write-set** — the
  agent colliding with its own lock. Recovery was a delete-ref + re-claim from the
  worktree (so `branch=perf/win-hunt`), plus a wasted push cycle.

  The confusing part: the lock and the branch are BOTH the operator's, so "plan-lock
  collision — overlaps the write set owned by a DIFFERENT branch" reads as if a
  second session is contending, when really it's a self-inflicted branch mismatch.

  Concrete next action (pick one):
  1. **Warn on tree/branch mismatch:** in `lock-claim.sh`, if the current branch is
     the repo's default/integration branch (`develop`/`main`) — an unlikely branch
     to hold a feature plan-lock — emit a loud "claiming lock owner=<branch>; you
     usually claim from the feature worktree, not the integration tree" note before
     the push. Cheapest, non-breaking.
  2. **Let the branch be explicit:** accept an optional `--branch <name>` (or
     `LOCK_CLAIM_BRANCH` env) so the caller pins the intended owner regardless of
     which tree runs the script — mirrors the worktree-per-session model.
  3. **Doc the gotcha** in `docs/perforce/AGENT_FLOWS.md` / the plan-lock section:
     "claim the lock from the SAME worktree that will push, so owner == pushing
     branch." (Do this regardless of 1/2.)

  Cross-ref: session PR #1632 (perf-win-hunt) — the lock claimed on `develop` blocked
  the `perf/win-hunt` push until released + re-claimed from the worktree.
  Resolution: applied — satisfied by the explicit `LOCK_BRANCH` env override (`lock-claim.sh` header + `branch=` resolution, the entry's option 2) plus the `docs/agent-rules/ship-loops.md:140-145` mandate to pass `LOCK_BRANCH` explicitly from the worktree HEAD with the detached-HEAD skip (option 3); option 1's loud integration-branch warning added to `lock-claim.sh` in this archival PR.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · claude-code · [tooling] · P3 — required-context "teeth" check: can this required context ever red a PR?

  Details: `test-required-context-parity.sh` verifies each
  `branch_protection.required_contexts` name matches a workflow job `name:`
  (byte-exact) — but not whether that job can EVER fail a PR. The all-gates-blocking
  review found a required context (`C++ lint`) that structurally could not fail
  (job-level mask + `cppcheck --error-exitcode=0`) and two (`High-integrity
  baseline/narrowing`) that always skip on PRs (`if: github.event_name == 'push'`)
  — required checks implying protection that doesn't exist. Add a heuristic warn:
  a required context whose hosting job is `if:`-gated to exclude `pull_request`,
  OR whose every failing path is masked, is a NO-OP gate. Emit WARN (not FAIL —
  a skip-on-PR job is legitimately vacuously-satisfied for merge-queue readiness),
  naming the vacuous contexts so a human confirms intent. Home: extend
  `test-required-context-parity.sh`. Cross-ref: shipped/all-gates-blocking.md § Deviations.
  Resolution: applied — the pr_triggered teeth shipped in `agents/scripts/core/test-required-context-parity.sh` (:93-102; selftest :190-191 asserts a push-only job hosting a required context FAILs — stronger than the proposed WARN); the residual every-failing-path-is-masked heuristic stays tracked by the sibling `tooling/2026-07-05-gate-lane-no-job-level-continue-on-error.md` entry (single tracker, no dual bookkeeping).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-05 · orchestrator (recurring-findings gate campaign #1605 ship session) · [tooling] · P2 — `native-automerge-bypasses-merge-snapshot-ledger`: a PR merged via GitHub's native auto-merge appended no line to `docs/self-improvement/merge-snapshots.jsonl`, so the ADR-0017 lossless merge-time capture had a hole for exactly the merge path a session without the `gh` CLI ends up arming.
  Details: ship-loops.md § merge-snapshot mandated the append for three actors (in-session orchestrator REST merge, `git-janitor`, `merge-watcher handle_pass()`), but a session that ARMS GitHub-native auto-merge is none of them — the merge fires server-side, possibly after the session goes idle, and nothing writes the row. Hit live on #1605/#1608 (cloud session; no `gh`, so `safe-merge.sh` could not run; full gate set hand-verified before arming). Sweeper-workflow alternatives were evaluated and rejected: a GITHUB_TOKEN workflow can neither push develop (required-status-check protection, non-bypass actor) nor open harvest PRs that trigger the required contexts (GITHUB_TOKEN-created PRs spawn no workflow runs), and a scheduled retro-composer would write confidently-wrong rows from an already-rewritten rollup — a stale line is worse than a hole, since `postmortem-owed.sh` reads the ledger BEFORE the live fallback.
  Resolution: applied — ship-loops.md § merge-snapshot gained the **fourth writer**: the session that armed the auto-merge appends the row on receiving the merged notification (PR-activity webhook / check-in), fetching `mergeCommit`/`headSha`/labels via MCP when `gh` is absent, calling the same idempotent helper with `mergeActor=orchestrator-automerge` + `SNAPSHOT_MERGED_AT=<mergedAt>`, and landing it in its next develop-bound commit. The #1605 + #1608 rows were seeded through exactly that path in the same PR. Residual (accepted, documented in the mandate): a session that dies before the merge event, or with no further develop-bound commit, leaves the hole to ADR-0017's live fallback — best-effort, never blindness; no retro-composition.
  Status: applied
  Last-reviewed: 2026-07-05

- 2026-07-04 · orchestrator (remote-session ship-loop) · [process] · P1 — a draft PR wedged the daemon-free autonomous merge path: `safe-merge.sh` never flipped draft→ready, so under the standing `governance.auto_merge: on` grant the loop paused on a PR the harness opened draft
  Details: The watcher daemon's first step on a registered PR is `ensure_pr_ready_for_review` (C4 prong 1), but the daemon-free path — the orchestrator driving `safe-merge.sh` in-session, the ONLY autonomous-merge path on remote/web sessions where no daemon persists — left `MERGE_GATES_FLIP_READY` unset. Remote/web harnesses open PRs DRAFT by default, so the sequence was: CodeRabbit skips the draft (`auto_review.drafts: false`), the CR gate blocks on NONE past the grace window, `safe-merge.sh` REFUSES, and the "autonomous" loop pauses on a state that never self-resolves — and even a CR-exempt pass would then fail the arm step (`gh pr merge` refuses drafts). The authorization model already covered this (invoking safe-merge IS the merge authorization, per AGENTS.md § Merge gates), but the flip was left to the caller's memory instead of the wrapper's contract.
  Concrete next action: applied — `safe-merge.sh` now defaults `MERGE_GATES_FLIP_READY=true` when unset (explicit caller values, including `false`, preserved for poll-only semantics) and runs `gh_pr_ready_idempotent` once more immediately before arming (mirrors the watcher's pre-merge flip). Selftest CASES 12–13 + two bats cases pin the default and the opt-out; documented in `merge-gates.md` (§ Draft never pauses an authorized merge), `ship-loops.md` (§ standing grant bullet), and the AGENTS.md § Merge gates one-liner.
  Status: applied (2026-07-04 — fix(merge): safe-merge defaults draft→ready flip so a draft PR never pauses an authorized autonomous merge)
  Last-reviewed: 2026-07-04

- 2026-07-04 · orchestrator (PR #1603 CI triage) · [infra] · P2 — Mobile advisory lanes red on every PR since the cpp-httplib bump: cached `.fetchcontent-src` lacks the new pinned ref and `UPDATE_DISCONNECTED` forbids fetching it
  Details: `Mobile — POSIX core compile gate (Linux clang, advisory)` and `Mobile — Android NDK arm64-v8a (.so configure+link, advisory)` both fail at configure with `Requested git ref "2132205e1a69c9fce8096f085b1b8d72efc759fa" is not present locally, and not allowed to contact remote due to UPDATE_DISCONNECTED` (FetchContent `httplib-populate`, `CMakeLists.txt:606`). Mechanism: the lanes restore a FetchContent source cache saved BEFORE #1588 bumped the cpp-httplib pin; the cached checkout doesn't contain the new ref, and `UPDATE_DISCONNECTED` turns the would-be re-fetch into a hard configure error. Observed on PR #1603 (a shell/docs/bats-only diff that cannot influence FetchContent), head 43956b1. develop's own latest push run skipped these lanes (docs-only change detection), so the red is invisible on develop and taxes every code-running PR instead.
  Concrete next action: include the dependency-pin in the lanes' FetchContent cache key (e.g. hash of the `CMakeLists.txt` FetchContent block or the pinned SHA) so a pin bump invalidates the cache, OR drop `UPDATE_DISCONNECTED` for cache-restored sources so a missing ref re-fetches instead of hard-failing. Until then these two advisory reds on unrelated PRs are this known infra issue, not the PR's diff.
  Resolution: applied — `CMakeLists.txt:479-487` sets `FETCHCONTENT_UPDATES_DISCONNECTED=OFF` when `ENV{CI}` is defined ('stale restored caches self-heal' — the entry's second remedy verbatim) while local/IDE configures keep the disconnected fast path; both mobile lanes are now blocking required contexts (`project.config.json` `required_contexts`), so a recurrence cannot hide as advisory noise.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-04 · orchestrator (CPP_CODE_AUDIT.md remediation, #1593) · [tooling] · P3 — writing a `SMATCHET_DEVIATION(rule=duplication; ...)` marker to suppress a copy-paste-clone finding took 2-3 iterations to fit under the 120-column line limit at least 4 separate times in one PR, because the natural wording for `reason=`/`owner=`/`revisit=` overruns the budget before the fields are even done
  Details: the dup-audit tool's suppression check requires the marker comment to live on a single physical line (or the single line containing "rule=duplication"); `clang-format`'s `ReflowComments` will wrap any comment line over ~120 columns onto a second line, which breaks the suppression match even though the marker was written correctly. Every time this PR added a `SMATCHET_DEVIATION(rule=duplication; ...)` marker (`TrackerFieldCatalog.cpp`, `PlaneIssueMutation.cpp`, `AttachmentAppUpdateService.cpp`'s pre-existing markers re-shortened after an unrelated edit re-triggered `clang-format` on the file, `SmatchetToolbarUi.cpp`), the first-attempt wording — a natural-language `reason=` clause plus `owner=cpp-audit; revisit=<date>` — landed at 130-180 columns and had to be shortened 1-2 more times (first attempt often still too long even after an initial trim) before `test-lint-rules.sh`/`pre-ship.sh` passed. This is pure iteration waste: the fix is always the same shape (terser `reason=`), so the budget could be known up front instead of discovered by repeated gate failures.
  Concrete next action: document the working budget directly at the point of use — either a one-line comment near `SMATCHET_DEVIATION`'s definition/grammar doc (likely `cpp-rules.md` or wherever the grammar is specified) stating "the full marker line, including the `// ` prefix and `owner=`/`revisit=` suffix, must fit in 120 columns — budget roughly 60-70 characters for `reason=` and keep it to a terse noun phrase (e.g. `reason=ParseBounded clone #8` not a full sentence explaining why)", or add a `--selftest`/lint-time hint that suggests a shortened `reason=` when a `SMATCHET_DEVIATION(rule=duplication; ...)` line is rejected purely for length (as opposed to missing/malformed fields). Either would turn a 2-3-iteration gate-fight into a single correct first attempt.
  Resolution: applied — column-budget note added to docs/agent-rules/cpp-rules.md § SMATCHET_DEVIATION grammar: full marker line incl. // prefix and owner=/revisit= must fit 120 columns (clang-format ReflowComments otherwise wraps it and breaks the suppression match); budget ~60-70 chars for a terse noun-phrase reason=. The optional dup_audit.py length-rejection hint was not taken (docs at point of use judged sufficient).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-04 · orchestrator (remote-session ship-loop) · [process] · P1 — a draft PR wedged the daemon-free autonomous merge path: `safe-merge.sh` never flipped draft→ready, so under the standing `governance.auto_merge: on` grant the loop paused on a PR the harness opened draft
  Details: The watcher daemon's first step on a registered PR is `ensure_pr_ready_for_review` (C4 prong 1), but the daemon-free path — the orchestrator driving `safe-merge.sh` in-session, the ONLY autonomous-merge path on remote/web sessions where no daemon persists — left `MERGE_GATES_FLIP_READY` unset. Remote/web harnesses open PRs DRAFT by default, so the sequence was: CodeRabbit skips the draft (`auto_review.drafts: false`), the CR gate blocks on NONE past the grace window, `safe-merge.sh` REFUSES, and the "autonomous" loop pauses on a state that never self-resolves — and even a CR-exempt pass would then fail the arm step (`gh pr merge` refuses drafts). The authorization model already covered this (invoking safe-merge IS the merge authorization, per AGENTS.md § Merge gates), but the flip was left to the caller's memory instead of the wrapper's contract.
  Concrete next action: applied — `safe-merge.sh` now defaults `MERGE_GATES_FLIP_READY=true` when unset (explicit caller values, including `false`, preserved for poll-only semantics) and runs `gh_pr_ready_idempotent` once more immediately before arming (mirrors the watcher's pre-merge flip). Selftest CASES 12–13 + two bats cases pin the default and the opt-out; documented in `merge-gates.md` (§ Draft never pauses an authorized merge), `ship-loops.md` (§ standing grant bullet), and the AGENTS.md § Merge gates one-liner.
  Status: applied (2026-07-04 — fix(merge): safe-merge defaults draft→ready flip so a draft PR never pauses an authorized autonomous merge)
  Last-reviewed: 2026-07-04

- 2026-07-04 · orchestrator (PR #1603 CI triage) · [infra] · P2 — Mobile advisory lanes red on every PR since the cpp-httplib bump: cached `.fetchcontent-src` lacks the new pinned ref and `UPDATE_DISCONNECTED` forbids fetching it
  Details: `Mobile — POSIX core compile gate (Linux clang, advisory)` and `Mobile — Android NDK arm64-v8a (.so configure+link, advisory)` both fail at configure with `Requested git ref "2132205e1a69c9fce8096f085b1b8d72efc759fa" is not present locally, and not allowed to contact remote due to UPDATE_DISCONNECTED` (FetchContent `httplib-populate`, `CMakeLists.txt:606`). Mechanism: the lanes restore a FetchContent source cache saved BEFORE #1588 bumped the cpp-httplib pin; the cached checkout doesn't contain the new ref, and `UPDATE_DISCONNECTED` turns the would-be re-fetch into a hard configure error. Observed on PR #1603 (a shell/docs/bats-only diff that cannot influence FetchContent), head 43956b1. develop's own latest push run skipped these lanes (docs-only change detection), so the red is invisible on develop and taxes every code-running PR instead.
  Concrete next action: include the dependency-pin in the lanes' FetchContent cache key (e.g. hash of the `CMakeLists.txt` FetchContent block or the pinned SHA) so a pin bump invalidates the cache, OR drop `UPDATE_DISCONNECTED` for cache-restored sources so a missing ref re-fetches instead of hard-failing. Until then these two advisory reds on unrelated PRs are this known infra issue, not the PR's diff.
  Resolution: applied — `CMakeLists.txt:479-487` sets `FETCHCONTENT_UPDATES_DISCONNECTED=OFF` when `ENV{CI}` is defined ('stale restored caches self-heal' — the entry's second remedy verbatim) while local/IDE configures keep the disconnected fast path; both mobile lanes are now blocking required contexts (`project.config.json` `required_contexts`), so a recurrence cannot hide as advisory noise.
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-04 · orchestrator (CPP_CODE_AUDIT.md remediation, #1593) · [tooling] · P3 — writing a `SMATCHET_DEVIATION(rule=duplication; ...)` marker to suppress a copy-paste-clone finding took 2-3 iterations to fit under the 120-column line limit at least 4 separate times in one PR, because the natural wording for `reason=`/`owner=`/`revisit=` overruns the budget before the fields are even done
  Details: the dup-audit tool's suppression check requires the marker comment to live on a single physical line (or the single line containing "rule=duplication"); `clang-format`'s `ReflowComments` will wrap any comment line over ~120 columns onto a second line, which breaks the suppression match even though the marker was written correctly. Every time this PR added a `SMATCHET_DEVIATION(rule=duplication; ...)` marker (`TrackerFieldCatalog.cpp`, `PlaneIssueMutation.cpp`, `AttachmentAppUpdateService.cpp`'s pre-existing markers re-shortened after an unrelated edit re-triggered `clang-format` on the file, `SmatchetToolbarUi.cpp`), the first-attempt wording — a natural-language `reason=` clause plus `owner=cpp-audit; revisit=<date>` — landed at 130-180 columns and had to be shortened 1-2 more times (first attempt often still too long even after an initial trim) before `test-lint-rules.sh`/`pre-ship.sh` passed. This is pure iteration waste: the fix is always the same shape (terser `reason=`), so the budget could be known up front instead of discovered by repeated gate failures.
  Concrete next action: document the working budget directly at the point of use — either a one-line comment near `SMATCHET_DEVIATION`'s definition/grammar doc (likely `cpp-rules.md` or wherever the grammar is specified) stating "the full marker line, including the `// ` prefix and `owner=`/`revisit=` suffix, must fit in 120 columns — budget roughly 60-70 characters for `reason=` and keep it to a terse noun phrase (e.g. `reason=ParseBounded clone #8` not a full sentence explaining why)", or add a `--selftest`/lint-time hint that suggests a shortened `reason=` when a `SMATCHET_DEVIATION(rule=duplication; ...)` line is rejected purely for length (as opposed to missing/malformed fields). Either would turn a 2-3-iteration gate-fight into a single correct first attempt.
  Resolution: applied — column-budget note added to docs/agent-rules/cpp-rules.md § SMATCHET_DEVIATION grammar: full marker line incl. // prefix and owner=/revisit= must fit 120 columns (clang-format ReflowComments otherwise wraps it and breaks the suppression match); budget ~60-70 chars for a terse noun-phrase reason=. The optional dup_audit.py length-rejection hint was not taken (docs at point of use judged sufficient).
  Status: applied
  Last-reviewed: 2026-07-08

- 2026-07-02 · orchestrator (PR #1593 CI failure) · [tooling] · P2 — `coverage-delta-gate.sh`'s test-light exemption pre-check crashes with exit 141 (SIGPIPE) instead of reporting a clean gate failure
  Details: `_classify_diff` deliberately `break`s out of its `while read` loop on the first real-runtime-surface line (by design — it only needs one counterexample to know the diff isn't exemptable). When it was fed via a `|` pipe (`git diff ... | _classify_diff`) under `set -euo pipefail`, that early `break` closes the reader's end of the pipe before `git diff` finishes writing; `git diff` then gets SIGPIPE and exits 128+13=141, `pipefail` propagates that 141 through the `EXEMPTION="$(...)"` assignment, and `set -e` kills the whole script — so any real (non-exempt) `Source/Core/src/*.cpp` diff without a test delta crashed the "Test-delta gate" CI check with a bare `Process completed with exit code 141` instead of reaching the intended `FAIL: Source/Core/ changes without test deltas.` message with remediation instructions. Reproduced + confirmed the mechanism with a minimal repro (early-`break` pipe reader under `set -o pipefail` reliably yields 141; the same reader via process substitution `reader < <(producer)` yields 0, since `pipefail`/`$?` don't track a process-substitution's background writer). Same defect class as the `pipefail var=$(...|head)` SIGPIPE/truncation guard added to `test-shell-lint.sh` Rule 6 (PR #1420) — but `coverage-delta-gate.sh` postdates that sweep and wasn't covered by it.
  Concrete next action: done — fixed in the PR that hit it (alexandrosk0/Smatchet#1593), in two passes. First pass changed `git diff ... | _classify_diff` to `_classify_diff < <(git diff ...)` (process substitution instead of a pipe) so an early `break` in the reader can no longer SIGPIPE the writer through `pipefail` — but this traded away `git diff`'s own exit-status propagation entirely: a bad `MERGE_BASE`/git error would now produce empty input, which `_classify_diff` reports as `EXEMPT`, silently PASSING a gate that should hard-fail. CodeRabbit's review of the PR caught this regression before merge. Final fix: write the diff to a `mktemp` temp file first (`git diff ... >"$GIT_DIFF_TMPFILE" 2>/dev/null`), check its exit status explicitly with `if ! ...; then FAIL; fi`, then feed the file to `_classify_diff` — no pipe (so no SIGPIPE risk) and no lost exit code, with a `trap ... EXIT` for cleanup. Follow-up: extend `test-shell-lint.sh` Rule 6 (or add a sibling rule) to also flag `<producer> | <fn-with-early-break>` shapes generically, not just the `$(...|head)` shape — this exact "early-break reader on a live pipe" pattern is the general case and will recur in future gate scripts. A second, narrower rule worth considering: flag a process-substitution `< <(producer ...)` feeding a function/loop that exits early, since that shape reliably drops the producer's own exit-status observability — the same trap this fix fell into on its first pass.
  Status: applied (fixed inline in #1593, verified regression-free by an independent review pass after CodeRabbit's catch; the generic shell-lint rule extensions above are the deferred follow-up)
  Last-reviewed: 2026-07-03

- 2026-07-02 · orchestrator (PR #1593 CI failure) · [tooling] · P2 — `coverage-delta-gate.sh`'s test-light exemption pre-check crashes with exit 141 (SIGPIPE) instead of reporting a clean gate failure
  Details: `_classify_diff` deliberately `break`s out of its `while read` loop on the first real-runtime-surface line (by design — it only needs one counterexample to know the diff isn't exemptable). When it was fed via a `|` pipe (`git diff ... | _classify_diff`) under `set -euo pipefail`, that early `break` closes the reader's end of the pipe before `git diff` finishes writing; `git diff` then gets SIGPIPE and exits 128+13=141, `pipefail` propagates that 141 through the `EXEMPTION="$(...)"` assignment, and `set -e` kills the whole script — so any real (non-exempt) `Source/Core/src/*.cpp` diff without a test delta crashed the "Test-delta gate" CI check with a bare `Process completed with exit code 141` instead of reaching the intended `FAIL: Source/Core/ changes without test deltas.` message with remediation instructions. Reproduced + confirmed the mechanism with a minimal repro (early-`break` pipe reader under `set -o pipefail` reliably yields 141; the same reader via process substitution `reader < <(producer)` yields 0, since `pipefail`/`$?` don't track a process-substitution's background writer). Same defect class as the `pipefail var=$(...|head)` SIGPIPE/truncation guard added to `test-shell-lint.sh` Rule 6 (PR #1420) — but `coverage-delta-gate.sh` postdates that sweep and wasn't covered by it.
  Concrete next action: done — fixed in the PR that hit it (alexandrosk0/Smatchet#1593), in two passes. First pass changed `git diff ... | _classify_diff` to `_classify_diff < <(git diff ...)` (process substitution instead of a pipe) so an early `break` in the reader can no longer SIGPIPE the writer through `pipefail` — but this traded away `git diff`'s own exit-status propagation entirely: a bad `MERGE_BASE`/git error would now produce empty input, which `_classify_diff` reports as `EXEMPT`, silently PASSING a gate that should hard-fail. CodeRabbit's review of the PR caught this regression before merge. Final fix: write the diff to a `mktemp` temp file first (`git diff ... >"$GIT_DIFF_TMPFILE" 2>/dev/null`), check its exit status explicitly with `if ! ...; then FAIL; fi`, then feed the file to `_classify_diff` — no pipe (so no SIGPIPE risk) and no lost exit code, with a `trap ... EXIT` for cleanup. Follow-up: extend `test-shell-lint.sh` Rule 6 (or add a sibling rule) to also flag `<producer> | <fn-with-early-break>` shapes generically, not just the `$(...|head)` shape — this exact "early-break reader on a live pipe" pattern is the general case and will recur in future gate scripts. A second, narrower rule worth considering: flag a process-substitution `< <(producer ...)` feeding a function/loop that exits early, since that shape reliably drops the producer's own exit-status observability — the same trap this fix fell into on its first pass.
  Status: applied (fixed inline in #1593, verified regression-free by an independent review pass after CodeRabbit's catch; the generic shell-lint rule extensions above are the deferred follow-up)
  Last-reviewed: 2026-07-03
