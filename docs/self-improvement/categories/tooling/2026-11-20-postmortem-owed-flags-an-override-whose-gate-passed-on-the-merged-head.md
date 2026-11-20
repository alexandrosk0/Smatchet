# `postmortem-owed.sh` flags a `plan-lock-out-of-band` label whose gate passed on the merged head

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-20
- **Observed on**: PR #2443 (merged 2026-11-20T00:43:43Z as `8cf3bb91a`), in the 2026-11-20 entry in [`postmortems.md`](../../postmortems.md); PR #2440 (merged 2026-11-20T15:30:36Z as `79baaa7a4`) and PR #2441 (merged 2026-11-20T04:45:35Z as `3d1ff166f`), both with `Plan-lock gate` green on the merged head, in the [later 2026-11-20 entry](../../postmortems.md#2026-11-20--pr-2440-2441--override-cr-out-of-band--plan-lock-out-of-band-on-both-coderabbit-skipped-on-both-merged-heads-plan-lock-gate-green-on-both).
- **Status**: open

## What happened

#2443 got `plan-lock-out-of-band` at 2026-11-19T13:19:28Z. At that point `Plan-lock gate` was red: two workflow files overlapped `ci-mobile-ubuntu-24-04-pin`, the lock #2454 held for the same runner pin. #2443 then reverted its own copy of the pin, #2454 merged, and the lock was released. `Plan-lock gate` on #2443's merged head (`6cf8ae608`) passed, so at merge the label waived nothing.

The SessionStart nudge still listed `PR #2443 — override: plan-lock-out-of-band`. That cost a postmortem with nothing to analyse.

## Why

`snapshot_trigger` in `agent-layer/agents/scripts/core/postmortem-owed.sh` emits `override: <label>` for every entry in the row's `overrideLabels`, whatever `redChecks` says. The live fallback works the same way from the PR's labels.

For `plan-lock-out-of-band`, the check is known: the label waives `Plan-lock gate` and nothing else. The row's `redChecks` already says whether that check was red. `safe-admin-merge.sh`'s `downgraded_red_checks` puts `Plan-lock gate` there when it is red, as #2432's row shows.

## Proposed fix

In `snapshot_trigger`, drop the `override: plan-lock-out-of-band` part when `redChecks` does not contain `Plan-lock gate`, and only for a row whose `gates` is `GATES_PASSED`. That verdict is written at the decision instant, so its `redChecks` is lossless.

Keep the trigger in two cases:
- for a `BACKFILLED` row, whose `redChecks` was read from the live rollup after the merge (lossy, ADR-0017);
- on the live-fallback path.

Leave `cr-out-of-band` as it is. The CodeRabbit signal is spread over a check run, a status context and review states, and `redChecks` does not capture all of it. #2451 and #2454 both record `redChecks: []` under a real waiver.

Pin the change with a bats case. A `GATES_PASSED` row with `overrideLabels: ["plan-lock-out-of-band"]` and `redChecks: []` yields no trigger. The same row with `redChecks: ["Plan-lock gate"]`, or as `BACKFILLED`, still yields `override: plan-lock-out-of-band`.

**Enumerator + replay**: the enumerator is the rows of `docs/self-improvement/merge-snapshots.jsonl` where `overrideLabels` contains `plan-lock-out-of-band`.
- #2432's `GATES_PASSED` row has `Plan-lock gate` in `redChecks`, so it stays flagged, which is correct.
- #2443's row is `BACKFILLED`, so this fix would still flag it. Only a merge through a sanctioned actor (`safe-merge.sh`, the merge-watcher) gets the precision, which is one more reason to merge through one.
