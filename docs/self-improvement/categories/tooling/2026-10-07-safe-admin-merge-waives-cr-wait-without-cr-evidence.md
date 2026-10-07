# `safe-admin-merge.sh` waives the CodeRabbit wait even when CodeRabbit never ran on the head

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-07
- **Observed on**: the code-review pass over alexandrosk0/the-unwilling-agentic-bunch#5. That PR made `evaluate_cr` require `cr-disposition` alongside `cr-out-of-band`.
- **Status**: open

## What happened

The merge-gate poller refuses a `cr-out-of-band` downgrade when there is no evidence that CodeRabbit ran on the head (`merge-gates.md` § `cr-out-of-band`, the merge-pipeline-04 residual).

`safe-admin-merge.sh` does not call the poller. Its `evaluate_cr` passes on `cr-out-of-band` + `cr-disposition` even when the rollup has no CodeRabbit row at all. Selftest CASE10's fixture is exactly that shape. So the admin-merge path can skip a review that never happened, where the poller would wait for it.

## Concrete next action

Make `evaluate_cr` take the poller's rule. The waiver passes only when a CodeRabbit row is present on the head, in any state; with no CR row it falls through to the grace backstop or BLOCK. Alternatively, share one jq predicate between the poller and `safe-admin-merge.sh`.

Update CASE10 to carry a CR row. Add a case that the waiver with no CR row does not pass. Add matching cases in `safe_admin_merge.bats`.
