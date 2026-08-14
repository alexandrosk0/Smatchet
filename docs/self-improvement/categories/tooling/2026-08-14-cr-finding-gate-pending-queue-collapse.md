# `cancel-in-progress: false` still collapses the PENDING queue — cr-finding-gate needs a decision

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-14
- **Split from**: `required-check-cancelled-while-pending-wedges-poller`
  (2026-08-04, archived — its problem (2), filed separately per its own scoping
  because this needs a workflow-design decision, not a poller change)

## Problem

`.github/workflows/cr-finding-gate.yml` sets `concurrency.group` per PR with
`cancel-in-progress: false`. The header comment reasons "let them all complete
rather than cancel" — true only for **in-progress** runs. GitHub keeps only ONE
**pending** run per group and cancels the rest; on #1937 five
`pull_request_review_comment` runs were cancelled in one second, and a
cancelled-while-pending run creates **no check-run at all**, leaving the
required `CR finding gate` context absent-forever (nothing re-triggers it).

The poller side is fixed (merge-gates.sh exit 8 names the cancelled run and the
`gh run rerun <id>` remedy), but the workflow still manufactures the wedge.

## Decision needed (either resolves it)

1. **Drop the `concurrency` block entirely.** The job only posts a status and
   the comment already argues last-write-wins; every run evaluates the current
   head state via GraphQL, so verdicts converge regardless of completion order.
   Cost: bursts run more concurrent jobs, each polling up to
   POLL_BUDGET_SECONDS (180 s) — runner minutes, not correctness.
2. **Keep it and add a backstop** (scheduled or `workflow_run`) that re-posts
   the context when the head lacks it. More moving parts; keeps burst cost low.

Option 1 is simpler and removes the class; take it unless runner-minute cost
is shown to matter.

## Status

Open.
