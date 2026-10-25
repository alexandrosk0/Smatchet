# `All checks green` stays red on a cancelled concurrency twin in a newer suite (correctly), but its red line did not say how to clear it

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-10-14 (corrected 2026-10-18)
- **Observed on**:
  - PR #2296, head `f30fa442`. Adding two labels at once (`cr-out-of-band`, `cr-disposition:cr-auto-review-disabled`, 18:10 UTC 2026-10-13) fired `perf-pr-fast.yml` twice. Run 38444613130 went on to pass `Detect perf-relevant changes` and `Perf PR-fast (windows-2022)`. Run 38444613295, created a moment later, was cancelled within a second by the workflow's concurrency group.
  - `All checks green` (run 38444613478) failed at 18:10 and again on a manual re-run at 20:33, both times naming only those two checks as `RED … (cancelled)`. Every other check on the head was green.
  - It passed at 21:24 only after the cancelled run 38444613295 was itself re-run (`rerun_workflow_run`) and succeeded.
- **Status**: fixed: the layer fix merged as the-unwilling-agentic-bunch#9 (`b60c11c`); the host pointer moves to it in alexandrosk0/Smatchet#2367

## Correction

The first version of this entry (#2343) called the aggregate's red a false red and proposed skipping a cancelled run when a non-cancelled run of the same name exists. That was wrong, and the proposed rule is unsafe:

- `Perf PR-fast (windows-2022)` is a required context (`project.config.json` § `branch_protection.required_contexts`).
- GitHub's required-check evaluation reads the newest workflow run for a check name. That is what made PR #2071 unmergeable with `405 Required status check "Perf PR-fast (windows-2022)" is cancelled` while the poller read green (`applied-2026-08.md`, the entry the poller's suite-aware dedup came from).
- So on #2296 the cancelled twin blocked the merge on GitHub's side too, and the aggregate's red was correct. Skipping the cancelled run would have reported green on a PR GitHub refuses to merge, recreating #2071's false green in the aggregate.

The recovery used on #2296, re-running the workflow run that owns the cancelled check, is the documented one: `agent-layer/docs/agent-rules/merge-gates.md` § "The `405 ... is cancelled` recovery".

## What was actually missing

The aggregate's red line said only `(cancelled)`. Re-running the aggregate cannot clear that state, and nothing on the line pointed at the recovery, so the obvious next step (re-run the red aggregate) was wasted. The merge-gate poller already warns about this shape before a merge; the aggregate did not.

Two labels added at once produce the shape routinely: each `labeled` event starts every workflow that triggers on it, and a `cancel-in-progress` concurrency group may cancel the run in the newer suite. The CodeRabbit and plan-lock waivers each take two labels.

## Fix

In the layer (`agent-layer/agents/scripts/core/all-checks-green.sh`), when a check's newest run is cancelled and an older run of the same name in a different suite did not end cancelled, the verdict stays red, and the line reads `cancelled in a newer workflow run, which GitHub evaluates; re-run that run: <its URL>`, pointing to the documented recovery. The check-runs fetch now carries `details_url` for that. Two bats cases pin it (the hint, and the plain `(cancelled)` line for a lone cancelled run or one over an older cancelled twin), and `merge-gates.md` describes the aggregate's line and the two-label trigger.

Not changed: the per-name pick itself. It matches GitHub's evaluation and must stay that way.
