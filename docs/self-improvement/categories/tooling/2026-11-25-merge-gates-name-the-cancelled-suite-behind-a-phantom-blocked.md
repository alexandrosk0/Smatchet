# `merge-gates.sh` does not name the cancelled check suite behind an all-green but BLOCKED PR

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-25
- **Observed on**: PR #2483 (head `c1b80e7df`, merged 2026-11-25T13:58:03Z as `1c0d9d6ef`), earlier on #2468 and #2482; in the 2026-11-25 entry in [`postmortems.md`](../../postmortems.md); later on #2494 and #2496, in the [2026-11-26 entry](../../postmortems.md#2026-11-26--pr-2488-2494--override-plan-lock-out-of-band-on-both-red-check-plan-lock-gate-on-2488-cr-out-of-band-on-2494)
- **Status**: open

## What happened

#2483 sat at `mergeStateStatus: BLOCKED` for about a day. Every required context was `success`, `statusCheckRollup.state` was `SUCCESS`, there were no review threads and no required reviews, and GitHub refused a plain squash merge ("base branch policy prohibits the merge"). The poll printed the same line each round: `BLOCK: GitHub mergeStateStatus=BLOCKED`, and it already warns about named stale contexts.

The blocker was two attempt-1 workflow runs, `Perf PR-fast` (45394384422) and `All checks green` (45394384325), cancelled by workflow concurrency when overlapping label and body events fired runs together. Later attempts passed, but the cancelled check suites (122829554677, 122829554299) stayed on the head. Neither showed up in the PR's check list, which collapses runs by name. They were found with `GET /commits/<sha>/check-suites`. Re-running the two runs one at a time cleared the state to CLEAN, and the PR merged with no rebase and no new CI.

## Why

`merge-gates.sh` reads the aggregated rollup, where the passing attempt hides the cancelled one. Its stale-context warning lists check names from the collapsed rollup, so it can miss a suite whose cancelled attempt is the only trace.

## Proposed fix

When the poll ends with `mergeStateStatus=BLOCKED` while the rollup is all green, list the head's check suites with `conclusion=cancelled` and print each suite's workflow run id and the command that clears it (`gh run rerun <id>`, one at a time, because concurrency cancels simultaneous reruns). Pin it with a bats case over a fixture of a green rollup plus one cancelled suite.

**Enumerator + replay**: the head's `check-suites` response, filtered to `conclusion == "cancelled"`. On #2483's head it yields exactly the two suites above.
