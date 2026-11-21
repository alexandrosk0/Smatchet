# A Perf PR-fast re-run was in flight when #2450 merged and failed after the merge

- **Category**: process
- **Priority**: P2
- **Date**: 2026-11-21
- **Observed on**: PR #2450 (merged 2026-11-20T21:41:34Z as `934601455`), in the 2026-11-21 entry in [`postmortems.md`](../../postmortems.md)
- **Status**: open

## What happened

`Perf PR-fast (windows-2022)` on #2450's merged head `dbd2e073e` has three runs. Two passed (02:47Z and 10:57Z). The third is a re-run of the 10:57Z run (run 44723562221): it started at 21:40:32Z, one minute before the merge, and finished `FAILURE` at 22:02:38Z, 21 minutes after it. The `postmortem-owed` nudge flagged `red-check: Perf PR-fast (windows-2022)`. The failing step was not identified: the tail of the failed-step log shows only checkout cleanup.

## Why

Unknown. Two readings fit: the merge poller read the rollup before the re-run replaced the earlier success, or the poller saw the re-run as pending and a path that does not wait for pending checks merged. The row in `merge-snapshots.jsonl` is `BACKFILLED` with `redChecks: []`, so the ledger cannot tell them apart (ADR-0017: backfilled projections are read from the live rollup after the merge).

## Proposed fix

1. Read the failed step of run 44723562221 (job 136586334932) and decide whether the failure is real or a runner flake.
2. Find out who started the 21:40Z re-run and why, and which merge path ran at 21:41Z.
3. If a re-run can start between the last poll and the merge, make the merge call re-read the rollup (the `--match-head-commit` guard does not cover a re-run on the same head).
