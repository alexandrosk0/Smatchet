# Merge-snapshot ledger has a 28-PR hole: no row written between #2067 and #2111

- **Category**: process
- **Priority**: P1
- **Date**: 2026-08-18
- **Found during**: appending the mandatory merge-time snapshot after
  [PR #2111](https://github.com/alexandrosk0/Smatchet/pull/2111) merged

## Symptom

`docs/self-improvement/merge-snapshots.jsonl` holds 147 rows. The last row before this
one is `#2067`; the next is `#2111`. Every PR merged in between has **no row**:

```
$ gh pr list --state merged --limit 60 --json number \
    -q '[.[] | select(.number > 2067 and .number < 2111)] | length'
28
```

All 28 (#2068 … #2108) merged between 2026-08-16T21:33Z and 2026-08-18T12:10Z, all with
`mergedBy = alexandrosk0`. Two full days of ship-loop activity left no gate-verdict
capture at all.

## Cause

Not yet diagnosed — this entry records the gap, not the fix. What is already known:

[`ship-loops.md:44`](../../../agent-rules/ship-loops.md) names four writers, and
`ship-loops.md:57` adds two that are code rather than rule (merge-pipeline-02):
`safe-admin-merge.sh` appends its own row, and `git-janitor.sh --post-merge` Step 5.5
backfills a row for a merge **no** actor recorded (verdict `BACKFILLED`, actor
`git-janitor`). That backfill is the designed net for exactly this case — a human/UI
merge — so the hole means either the in-session actor never reached its append **and**
`--post-merge` never ran, or it ran outside its age cap.

The cap is what makes this unrecoverable: Step 5.5 is bounded by
`SMATCHET_JANITOR_SNAPSHOT_MAX_AGE_HOURS` (default 6 h, undatable fails closed to skip),
and `ship-loops.md:55` prohibits retro-composing older rows outright — *"do not
retro-compose rows hours later from a possibly re-run rollup, a stale line is worse than
a hole."* Every one of the 28 is now past the cap. The data is gone, by design; only the
recurrence is fixable.

## Why it matters

The ledger exists because the merge-decision instant is the **only** lossless capture —
GitHub overwrites `statusCheckRollup` contexts on re-run and strips override labels
post-merge ([ADR-0017](../../../adr/0017-merge-time-snapshot-ledger.md)). A hole is not a
cosmetic gap in a log: it is the permanent loss of the evidence that a given merge was
gated, over precisely the window in which 10 `*-out-of-band` labels were left behind on
merged PRs (surfaced by the same closeout's `issue-sweep.sh` dry-run: #2105, #2100, #2096,
#2088, #2075, #2074, #2071, #2070, #2069 `cr-out-of-band`; #2097
`plan-lock-out-of-band`). Those are the merges whose override rationale most needed
capturing, and they are the merges with no row.

It is filed **P1** rather than P2 because the failure is silent and the loss is
irreversible: nothing warned that 28 consecutive merges skipped a step documented as
*mandatory*, and by the time the gap is visible in the file, the age cap has already
closed the only sanctioned repair. `postmortem-owed.sh` keeps a live `statusCheckRollup`
fallback for un-snapshotted merges, but that fallback reads current state — the exact
thing the ledger exists because it cannot trust.

## Proposed fix

Two independent gaps, either of which alone would have prevented this:

1. **Detect the hole while it is still repairable.** A SessionStart or closeout check
   comparing merged-PR numbers against ledger rows over the last N hours, nagging while
   the merges are inside the Step 5.5 age cap. The nag has to fire on the *gap*, not on
   any single session's behaviour — no session that skipped its own append is going to
   notice it skipped.
2. **Close the actor-taxonomy gap this closeout hit.** The valid `mergeActor` tokens are
   `orchestrator`, `git-janitor`, `merge-watcher`, `safe-admin-merge`,
   `orchestrator-automerge`. None describes what happened here: an in-session orchestrator
   polled the gates to `GATES_PASSED` and the **user** then merged through the GitHub UI.
   The row was written with `orchestrator` (accurate as the *writer* — that is the sense
   the `git-janitor`/`BACKFILLED` case already uses, where the actor did not merge either),
   but a reader cannot distinguish "the orchestrator merged" from "the orchestrator
   verified and a human merged". A distinct token would make that legible instead of
   leaving it to the reader.

Both are small; the value is that (1) turns a permanent hole into a recoverable one.

## Note on scope

The 28 missing rows were deliberately **not** backfilled — that is the retro-compose
prohibition above, and a fabricated row is worse than the hole it fills.
