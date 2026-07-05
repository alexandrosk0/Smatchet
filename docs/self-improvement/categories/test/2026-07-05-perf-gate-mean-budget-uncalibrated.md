# Perf gate is required but its mean-budget teeth are still unarmed (step-5 calibration owed)

- **Category:** test
- **Priority:** P2
- **Date:** 2026-07-05
- **Status:** open (plan: [`docs/plans/active/perf-gate-step5-calibration.md`](../../../plans/active/perf-gate-step5-calibration.md))

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
