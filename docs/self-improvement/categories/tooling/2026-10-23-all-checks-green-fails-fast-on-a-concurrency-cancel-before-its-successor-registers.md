# `All checks green` fails fast on a concurrency-cancelled run before the newer run of that workflow has registered

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-10-23
- **Observed on**: PR #2364, head `1f0554a2`.
  - Adding `cr-out-of-band` and `cr-disposition:cr-auto-review-disabled` together (02:48 UTC) fired `perf-pr-fast.yml` twice.
  - Run 39891559599 (#4126) was cancelled by the workflow's concurrency group at 02:48:17; run 39891559948 (#4127) passed `Perf PR-fast (windows-2022)` at 02:48:29.
  - `All checks green` run 39891559984 read the head at 02:48:28, before #4127's check run was listed. It saw only the cancelled one and ended `RED Perf PR-fast (windows-2022) (cancelled)` at once.
  - A plain re-run of the aggregate was the whole recovery: the newest run was already green.
- **Status**: open

## Why

`agent-layer/agents/scripts/core/all-checks-green.sh` exits 1 on the first blocking red (header: "first blocking red -> exit 1 at once (fail fast)"), and `cancelled` counts as red. Only an all-green verdict gets the `ACG_SETTLE_SECONDS` re-poll.

A run cancelled by a `cancel-in-progress` concurrency group always has a newer run of the same workflow behind it, but that run's check runs can appear a few seconds later. In that window, the cancelled run is the newest one the aggregate can see, so its red is not yet GitHub's verdict.

This is the opposite shape from `2026-10-14-all-checks-green-cancelled-concurrency-twin-hides-green-run.md`. There, the cancelled run was the newer one and the red was correct. Here, the cancelled run is the older one and the red is premature.

## Proposed fix

In the layer script, treat a newest-per-name check that concluded `cancelled` as pending, not red, until it has stayed the newest for one settle interval (`ACG_SETTLE_SECONDS`). Then fail as today. Any other red stays fail-fast.

A bats case should pin both shapes:
- cancelled, then a newer green run appears within the grace: green;
- cancelled with no successor after the grace: red, still pointing to the re-run recovery.

The cost is at most one settle interval of extra wait on a truly cancelled check.
