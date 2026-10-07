# The merge-gate poller passes a CodeRabbit review whose findings are all "outside the diff"

- **Category**: tooling
- **Priority**: P1
- **Date**: 2026-10-07
- **Observed on**: alexandrosk0/the-unwilling-agentic-bunch#5. At head 6a2fb92, `merge-gates.sh` reported `GATES_PASSED` with `CodeRabbit: COMMENTED (no Actionable header)`. CodeRabbit's review at that head carried **four "Outside diff range" findings, two of them Major**:
  - `review-ack.sh` branch mode skipped untracked files;
  - `safe-admin-merge.sh` let a bare `cr-out-of-band` waive the CodeRabbit wait.
  All four were real and are fixed in that PR.
- **Status**: open

## What happened

When a finding lands on lines outside the PR's diff, CodeRabbit cannot post it inline. It puts it in the review body under a `⚠️ Outside diff range comments (N)` block, and omits the `**Actionable comments posted: N**` header.

The poller reads two signals:
- that header, for the actionable count;
- unresolved review threads (`cr_open`).

Outside-diff findings produce neither. The review reads as a clean `COMMENTED`, and the PR passes the CodeRabbit gate with open Major findings. Auto-merge (`governance.auto_merge: on`) would have merged it.

## Concrete next action

In the CodeRabbit arm of `merge-gates.d/10-gate-filter.sh` (agent layer), parse the current-head review body for `Outside diff range comments (N)` with N > 0. Treat it like `COMMENTED + N > 0`: block, with `cr-out-of-band` + `cr-disposition` as the only waiver.

Mirror this in the host CR finding gate (`.github/actions/cr-finding-gate/action.yml`), which derives the actionable count the same way.

Bats coverage, in `merge_gates.bats` and `cr_finding_gate.bats`, using a fixture review body that has only an outside-diff block:
- it blocks;
- the same body without the block passes;
- a body with both an `Actionable comments posted: 1` header and an outside-diff block counts both.
