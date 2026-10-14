# `All checks green` keeps the newest check suite per name, so a cancelled concurrency twin in a newer suite hides a green run of the same check on the same head

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-14
- **Observed on**:
  - PR #2296, head `f30fa442`. Adding two labels at once (`cr-out-of-band`, `cr-disposition:cr-auto-review-disabled`, 18:10 UTC 2026-10-13) fired `perf-pr-fast.yml` twice. Run 38444613130 went on to pass `Detect perf-relevant changes` and `Perf PR-fast (windows-2022)`. Run 38444613295, created a moment later, was cancelled within a second by the workflow's concurrency group.
  - `All checks green` (run 38444613478) failed at 18:10 and again on a manual re-run at 20:33, both times naming only those two checks as `RED … (cancelled)`. Every other check on the head was green.
  - It passed at 21:24 only after the cancelled run 38444613295 was itself re-run (`rerun_workflow_run`) and succeeded, about 20 extra minutes of Windows perf CI that tested nothing new.
- **Status**: open

## What happened

`agent-layer/agents/scripts/core/all-checks-green.sh` (layer `33f2fa2`, the jq at `:167-168`) picks one run per check name:

```jq
group_by(.name // "")
| map(sort_by([(.check_suite.id // 0), (.id // 0)]) | .[-1])
```

and any `cancelled` conclusion counts as red (`:171`). The header (`:19-24`) assumes the newest suite is the run that superseded the others: "a re-run supersedes, a cancelled concurrency twin in an older suite does not count".

That assumption fails when two events land together. `perf-pr-fast.yml` runs on `labeled` and has `concurrency: { group: perf-pr-fast-${{ github.ref }}, cancel-in-progress: true }`. Two `labeled` events created two runs, and the concurrency group kept whichever entered it last. Here the one GitHub created first won. The loser was the run with the higher suite id, so the aggregate's pick was the cancelled twin, and the green run beside it was never looked at.

No human or code action caused the red. Labelling a PR, which the merge gates themselves ask for (`cr-out-of-band` + `cr-disposition`, `plan-lock-out-of-band` + `plan-lock-disposition`), is enough. Any workflow with `labeled` in its triggers and `cancel-in-progress: true` can do it, and the two-label waivers make it likely.

## Why it matters

`All checks green` is the one context meant to stand for "every check on this head is green" (block-on-any-red). A false red there blocks a mergeable PR. The obvious ways out are worse than the bug: re-running the aggregate does nothing (the cancelled twin is still the newest suite), and merging past a red aggregate is exactly what the merge rules forbid. The only clean exit today is re-running the cancelled twin, which costs a full CI lane per occurrence and is not documented anywhere.

## Concrete next action

1. **Do not let a cancelled run outrank a finished run of the same name on the same head.** In the per-name pick, prefer the newest run whose conclusion is not `cancelled`, and fall back to the newest run only when every run of that name was cancelled (then it stays red, as now):

   ```jq
   group_by(.name // "")
   | map(sort_by([(.check_suite.id // 0), (.id // 0)])
         | (map(select((.status // "") != "completed" or (.conclusion // "") != "cancelled")) | .[-1]) // .[-1])
   ```

   Pending runs stay eligible, so a queued or in-progress re-run still supersedes the result it replaces. A real failure is never hidden: a cancelled run can only lose to a run that finished (or is still running) for the same head SHA, and the aggregate already reads runs for that SHA only.
2. **Bats case** in the layer's all-checks-green suite: two runs of one name on the head, the older-suite one `success` and the newer-suite one `cancelled` → verdict pass. Keep the existing cases where a lone `cancelled` run, or a newer `failure` over an older `success`, stays red.
3. **Mirror the rule in the poller.** `agent-layer/agents/scripts/core/merge-gates.d/10-gate-filter.sh` makes the same per-name choice from GraphQL (`checkSuite.createdAt`), so it can hold the same false red; apply the same "skip cancelled when a non-cancelled sibling exists" rule there and pin it in `merge_gates.bats`.
4. **Then bump `agent-layer`** in this repo.

Until it lands: when `All checks green` names only `(cancelled)` checks and a same-name run on the head passed, re-run the cancelled workflow run (`rerun_workflow_run`), then re-run the aggregate. Do not merge past the red.
