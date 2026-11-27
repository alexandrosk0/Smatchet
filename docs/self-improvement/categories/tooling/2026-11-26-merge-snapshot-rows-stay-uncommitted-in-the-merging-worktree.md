# A merge-snapshot row stays uncommitted in whichever worktree armed the merge

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-11-26
- **Observed on**: PR #2423, #2431, #2436, #2447, #2485, #2488, #2492, #2497, #2498: each had a complete row in a worktree's working copy and none on `develop` (2026-11-26). Related, archived in `applied-2026-06.md`: `merge-snapshot-ledger-uncommitted-on-shared-tree-loss-risk`.
- **Status**: open

## What happened

`develop` had no snapshot row for 21 PRs merged between 2026-11-13 and 2026-11-26. For nine of them the merge actor had written the row (`mergeActor: orchestrator-automerge`, with `crState`), but only into the `docs/self-improvement/merge-snapshots.jsonl` working copy of the worktree it ran from: `awaiting-instructions-4d76da`, `review-other-sessions-5e4cdd`, `views-grid-ui-issues-7f7751`, `quizzical-franklin-25dc39` and `sharp-maxwell-2d04ea`. Nobody committed them. Removing or resetting any of those worktrees would have dropped the rows, and a backfill would then record `redChecks` from the live rollup instead of the decision instant.

## Why

The writer appends to a file in the caller's tree and stops there. Getting the row to `develop` is a separate manual step that the session which armed the merge often never takes, and the `merge-snapshot holes` nudge reads `develop` only, for a 6 h window.

## Proposed fix

Make the harvest find stranded rows: a `git-janitor` step (and the SessionStart nudge) that reads `git diff HEAD -- docs/self-improvement/merge-snapshots.jsonl` in every `git worktree list` entry and reports rows whose `pr` + `mergeCommit` are absent from `origin/develop`, so the next ledger PR copies them instead of backfilling. The worktree removal paths should refuse, or save the rows first, when such a row exists. Pin it with a bats case over a fixture repo with one linked worktree holding an uncommitted row.

**Enumerator + replay**: the loop above, over this host's worktrees on 2026-11-26, yields the nine PRs listed, plus a row for #2382 (merged 2026-11-03), which is also absent from `develop`.
