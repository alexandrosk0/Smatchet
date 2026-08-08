## [P1] CR finding gate: poll loop outlives its own job timeout, wedging the PR

**Category**: tooling
**Date**: 2026-08-06
**Observed on**: PR #1954

### What happened

`.github/actions/cr-finding-gate/action.yml` polled CodeRabbit with
`ATTEMPTS=12` + `sleep 15`. That bounds only the *sleeping* (165 s); the loop's
real cost is 165 s **plus 12 GraphQL round-trips**, which is unbounded from the
step's point of view. The job carried `timeout-minutes: 5`, shared with setup +
checkout.

On a congested runner the job was killed mid-`sleep`, **before either terminal
`post` ran**. Consequences, all silent:

- the required check-run `CR finding gate` never reached a terminal state;
- the StatusContext `CR findings (0 actionable)` kept whatever value it already
  had (stale);
- nothing re-triggers the workflow — its triggers are CR review/comment events,
  and CR had already spoken.

Net: the PR wedged with **no self-healing path**. Only a manual re-run cleared
it. The gate's entire contract is "always leave a status behind", and the one
failure mode that breaks that contract had no guard.

### Why the existing gates missed it

The two numbers that must be ordered (`POLL_BUDGET`/`ATTEMPTS` in the composite
action, `timeout-minutes` in the workflow) live in **different files**, with no
assertion tying them together. Nothing failed; the job simply died. A timeout
kill is not a red test — it looks like infrastructure noise.

### Preventing gate

`tests/bats/cr_finding_gate.bats` (wrapper
`agents/scripts/project/test-cr-finding-gate-bats.sh`) pins both invariants:

1. **Timeout ordering** — `POLL_BUDGET_SECONDS` < Evaluate step
   `timeout-minutes` < job `timeout-minutes`, so the poll window always closes
   with time left to post and the step always dies before the job.
2. **Fallback poster exists** — an `if: always()` step posts PENDING to the
   *same* StatusContext when `steps.eval.outcome` is neither `success` nor
   `skipped`, converting "wedged forever" into "pending, re-runnable".

Both carry negative selftest fixtures (inverted ordering; fallback removed), so
the checks are proven to fire rather than passing vacuously.

### Generalisable lesson

**A poll loop that bounds its sleeps has not bounded its runtime.** Bound the
wall clock (`SECONDS`-based deadline), not the attempt count — then the exit
time is an invariant of the step rather than a consequence of API latency.

**Step-scoped vs job-scoped timeouts are not interchangeable.** A step timeout
cancels one step and lets `if: always()` cleanup run; a job timeout takes the
cleanup down with it. Any workflow whose contract is "always post something"
needs its risky work step-scoped.
