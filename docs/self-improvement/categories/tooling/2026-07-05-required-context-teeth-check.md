- 2026-07-05 · claude-code · [tooling] · P3 — required-context "teeth" check: can this required context ever red a PR?

  Details: `test-required-context-parity.sh` verifies each
  `branch_protection.required_contexts` name matches a workflow job `name:`
  (byte-exact) — but not whether that job can EVER fail a PR. The all-gates-blocking
  review found a required context (`C++ lint`) that structurally could not fail
  (job-level mask + `cppcheck --error-exitcode=0`) and two (`High-integrity
  baseline/narrowing`) that always skip on PRs (`if: github.event_name == 'push'`)
  — required checks implying protection that doesn't exist. Add a heuristic warn:
  a required context whose hosting job is `if:`-gated to exclude `pull_request`,
  OR whose every failing path is masked, is a NO-OP gate. Emit WARN (not FAIL —
  a skip-on-PR job is legitimately vacuously-satisfied for merge-queue readiness),
  naming the vacuous contexts so a human confirms intent. Home: extend
  `test-required-context-parity.sh`. Cross-ref: shipped/all-gates-blocking.md § Deviations.
