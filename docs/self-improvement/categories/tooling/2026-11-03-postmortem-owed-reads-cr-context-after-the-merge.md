# `postmortem-owed.sh` judges a merged PR by a `CR findings` context posted after the merge

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-03
- **Observed on**: PR #2370 (merged 2026-11-01T14:43:38Z) and PR #2376 (merged 2026-11-02T00:57:38Z). Both are in the 2026-11-03 `cr-out-of-band` entry in [`postmortems.md`](../../postmortems.md).
- **Status**: open

## What happened

Both PRs merged through the gate poller with `cr-out-of-band` and a `cr-disposition:` label. When the PR was closed, the labels came off:
- #2370: `unlabeled` at 14:44:05Z, 27 s after the merge;
- #2376: `unlabeled` at 00:58:03Z, 25 s after the merge.

Each `unlabeled` event re-ran `CR finding gate`, which posted a new `pending` `CR findings (0 actionable)` context on the merged head, at 14:47:47Z and 01:01:13Z.

Run from a checkout whose `merge-snapshots.jsonl` lacked those PRs' rows (#2374 had not merged there yet), the SessionStart nudge reported both as `cr-context-not-green: PENDING`. Neither merge crossed a pending CR context: at merge time the newest one was `success` ("cr-out-of-band + cr-disposition set — gate overridden", 14:28:34Z and 00:55:08Z).

## Why

In `agent-layer/agents/scripts/core/postmortem-owed.sh`, field 9 of `JQ_ROWS` selects the latest `CR findings` StatusContext on the head by `createdAt`, with no `mergedAt` bound. Field 7 (`required-never-terminal`) already keeps only runs that existed at the merge, and its comment explains why: a post-merge re-run is not evidence about the merge.

Today the snapshot's `overrideLabels` usually masks the bug. But a PR whose CR context was green at the merge and was re-run afterwards, without a waiver and without a snapshot row, reads as an escape.

## Proposed fix

In the layer, filter field 9 like field 7. Keep only `CR findings` contexts whose `createdAt` is at or before `mergedAt`, and keep a context with no timestamp, because unknown is not the same as after. Pin it with a bats case: a merged head with a `success` context before the merge and a `pending` one after it is not flagged. The control case, `pending` before the merge, is still flagged.

**Enumerator + replay**: the enumerator is the head's status list (`GET repos/alexandrosk0/Smatchet/commits/<head>/statuses`, `CR findings` contexts), with `mergedAt` from `gh pr view`. Replayed on #2370 (`20548836c`) and #2376 (`0e24e946f`), the bounded pick is `success` on both, so neither is flagged. On #2371 (`772a5b854`) and #2380 (`c52ceeb58`), the pre-merge pick is still `pending`, so both stay flagged, correctly.
