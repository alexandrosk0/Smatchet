- 2026-11-02 · orchestrator · [test] · P3 — CR finding gate bats tests decide verdicts with a copy of the action's shell logic

  `tests/bats/cr_finding_gate.bats` runs the jq stages it extracts from
  `.github/actions/cr-finding-gate/action.yml` (check-run selection, the `coderabbitai` app filter, the
  status/conclusion to `cr_ctx` mapping). The step after that is not extracted: `verdict()` re-implements the
  SKIPPED to SUCCESS rewrite and decide()'s pass / fail / retry branches with its own `case`. A regression in
  the action's shell decision can leave every check-run test green. The SKIPPED-rewrite and GraphQL-query
  tests are literal greps. The model has drifted before: the `verdict()` comment records it diverged from
  decide() until the manual-review arm was added. Raised by CodeRabbit on #2379 (Trivial, heavy lift),
  declined there as out of that fix's scope.

  Concrete next actions:
  1. Move decide()'s `cr_ctx` / `cr_desc` decision block into a sourced helper script that both the action
     and the bats suite call, then delete `verdict()`'s copy.
  2. Or add an execution test that runs the action's real step body against stubbed `gh api graphql`
     responses built from a recorded `statusCheckRollup` payload, covering SKIPPED (must not pass),
     SUCCESS and FAILURE.

  Status: open
  Last-reviewed: 2026-11-02
