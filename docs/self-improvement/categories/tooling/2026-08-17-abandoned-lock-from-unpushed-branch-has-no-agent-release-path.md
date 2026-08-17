# A lock claimed by an unpushed branch has no release path an agent can reach

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-08-17
- **Found during**: shipping [PR #2085](https://github.com/alexandrosk0/Smatchet/pull/2085) (`fix-ui-perf-batch`)

## Symptom

`refs/locks/pillar2-ui-thread-config-io` was claimed at `2026-08-17T02:06:56Z` by branch
`claude/build-run-latest-8adc6a`. That branch was **never pushed**, and its
`originating_plan` (`docs/plans/active/pillar2-ui-thread-config-io.md`) is not on
`develop` either — the entire claim exists only as the lock ref.

Its 24-file write set overlapped `Views.cpp` and `SmatchetViewsDashboardUi.cpp`, so the
`Plan-lock gate` went red on PR #2085 and stayed red. Shipping needed the
`plan-lock-out-of-band` override.

Trying to clean it up afterwards:

```
$ bash agents/scripts/core/lock-release.sh pillar2-ui-thread-config-io
lock-release: delete of refs/locks/pillar2-ui-thread-config-io failed after 3 attempts:
error: RPC failed; HTTP 403 curl 22 The requested URL returned error: 403
```

## Cause

Three mechanisms each exclude this case, and together they leave no agent-reachable path:

| mechanism | why it does not apply |
|---|---|
| [`lock-release.sh`](../../../../agents/scripts/core/lock-release.sh) | Its header names this exact use — *"meant for abandoned branches or pre-cutover manual housekeeping"* — but the `plan-locks` ruleset ([`setup-locks-ruleset.sh`](../../../../agents/scripts/core/setup-locks-ruleset.sh)) restricts `refs/locks/*` deletion to `RepositoryRole/admin` + `Integration/github-actions`. A non-admin agent gets 403. |
| [`lock-cleanup.yml`](../../../../.github/workflows/lock-cleanup.yml) | Fires on `pull_request: closed` and reads `lock-slug:` from the PR body. An unpushed branch has no PR, so no close event will ever occur. |
| [`lock-staleness.yml`](../../../../.github/workflows/lock-staleness.yml) | Opens/updates an Issue only — it **never deletes** — and not until the 14-day threshold. |

So an abandoned reservation from a branch that was never pushed blocks every overlapping
PR for **14 days**, and then still only produces an Issue for a human.

The asymmetry is the root of it: claiming is cheap and unilateral (any agent, no review),
while releasing is admin-gated. A cheap-to-take, expensive-to-return lock accumulates.

## Why this is not the entries already on file

[`2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md`](2026-08-17-pr-body-rewrite-drops-lock-slug-marker.md)
and [`2026-08-16-commented-lock-slug-marker-silently-skips-release.md`](2026-08-16-commented-lock-slug-marker-silently-skips-release.md)
are both about the marker being **absent or malformed in a PR body**. Both presuppose a
PR exists. Here there is no PR and never will be — the detection surface those entries
propose (inspect the body at close time) cannot fire at all.

## Suggested fix

Ordered cheapest-first; the first is probably sufficient.

1. **Make the claim self-invalidating.** `plan-lock-gate.sh` already keys identity on
   branch. Have it treat a lock whose `branch` does not exist on the remote as
   non-blocking after a short grace (hours, not days) — the same "unattributable →
   non-blocking" reasoning the gate already applies to an empty/detached lock branch.
   This needs no new permissions and no deletion.
2. **Give the staleness sweep teeth for this shape specifically**: a lock whose claiming
   branch is absent from the remote is not merely stale, it is void — the sweep runs as
   `github-actions` and is already on the ruleset bypass list, so it *can* delete.
3. **A `workflow_dispatch` release workflow** taking a slug, running as `github-actions`.
   This is the general escape hatch; it wants an audit trail since it can release any lock.

## What was done here

Released via `lock-cleanup.yml` by putting `lock-slug: pillar2-ui-thread-config-io` in
this PR's body — the one agent-reachable path, and legitimate since this PR *is* the
disposition of that lock. Recorded for re-creation if that session returns:
the ref was `6906febefcc8891631e615187477ff015ce5c6e4`, claim
`{"branch":"claude/build-run-latest-8adc6a","owner":"orchestrator","started":"2026-08-17T02:06:56Z"}`,
notes *"clear 12 runtime Pillar-2 UI-thread config I/O violations"*.

Note that PR #2085 has since landed part of that write set: in `Views.cpp` and
`SmatchetViewsDashboardUi.cpp` the inline config writes are now
`smatchet::config_save::Enqueue*` calls, so the plan's remaining scope is 22 files, not 24.
