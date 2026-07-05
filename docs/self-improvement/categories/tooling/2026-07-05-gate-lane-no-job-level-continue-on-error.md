- 2026-07-05 · claude-code · [tooling] · P2 — lint: a non-"advisory"-named CI job must not carry job-level continue-on-error

  Details: the all-gates-blocking flip had THREE lanes drift out of sync between
  three coupled attributes — check name de-advisoried, step/job mask retained,
  required-context promoted (bucket-E, mobile-texture-guard, cpp-lint). The
  pre-ship code-review round caught them by hand (4 HIGH findings). A cheap
  mechanical gate would catch the class: scan `.github/workflows/*.yml` and FAIL
  if any job whose `name:` does NOT contain "advisory" (case-insensitive) sets
  job-level `continue-on-error: true`. Job-level masks green-wash the whole
  workflow run and are the anti-pattern the flip removed; step-level masks
  (the sanctioned per-step survivors: fuzz stochastic, bucket golden diff,
  bucket-E per-test, cpp-lint cppcheck) are exempt — the rule is job-level only.
  Home: `agents/scripts/project/test-lint-rules.sh` (new rule id
  `gate-job-mask-non-advisory`) or a standalone workflow-audit script wired into
  the "Doc anchors + agent contract" suite. Cross-ref: shipped/all-gates-blocking.md.
