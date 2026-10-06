# `Plan-lock gate` reds on a path overlap that a clean merge of the two pushed heads already shows is harmless

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-06
- **Observed on**:
  - PR #2313: merged under `plan-lock-out-of-band` over lock `shutdown-cancel-all-pane-syncs`, which overlapped `AppController.h`, `AppController_LocalCacheDb.cpp` and `tests/support/FakeTrackerClient.h`. See its [postmortem](../../postmortems.md#2026-10-06--pr-2313--override-plan-lock-out-of-band-over-a-live-lock-whose-three-overlapping-files-merged-clean-in-disjoint-hunks).
- **Status**: open

## What happened

Plan locks claim whole files, and `plan-lock-gate.sh` compares paths only. A lock that lists a widely shared file therefore reds every other PR that touches it while the lock lives, however far apart the two edits are. `Source/Core/include/AppController.h` (include fan-in baseline 70) and `tests/support/FakeTrackerClient.h` are such files.

#2313 added members and private methods to `AppController.h`, one line to the cache-recreate path in `AppController_LocalCacheDb.cpp` (reread the queue view from the new file), and scripted-fetch helpers to `FakeTrackerClient.h`. The lock holder, alexandrosk0/Smatchet#2291, changed other lines of the same three files. Both branches were on origin, and `git merge-tree --write-tree` of the two heads was clean in either order. That check had to be run by hand and recorded as a `plan-lock-disposition:`, and the merge still spent an override, while #2291 sat idle and red.

## Concrete next action

In `plan-lock-gate.sh`, after the path overlap is found and only for a lock whose `branch` is on origin:

- fetch that branch's head;
- run `git merge-tree --write-tree <PR head> <lock branch head>`;
- when the merge is clean, report the overlap as a WARN naming the slug, the files and both heads, and do not fail the job.

It stays red when the merge conflicts, when the lock's branch is not on origin (its edits are unseen, so the claim has to stand in for them), and when the lock is not attributable. The comparison basis stays the declared write set: the merge only relaxes an overlap the write set already found, so the gate does not start reacting to other open PRs.

Bats coverage:

- a disjoint edit to a claimed file merges clean and passes with a WARN;
- an overlapping hunk in the same file stays red;
- a claim whose branch is absent from origin stays red.

Replayed on #2313, the gate gives three WARNs and no red.

Separately, `lock-claim.sh` should drop write-set entries under `.claude/worktrees/<id>/`. `shutdown-cancel-all-pane-syncs` lists every path twice, once with that prefix, and those entries can never match a repo path.
