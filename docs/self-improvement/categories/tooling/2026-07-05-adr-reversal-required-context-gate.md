- 2026-07-05 · claude-code · [tooling] · P3 — doc-validation: flag a required_contexts addition that an ADR explicitly rejected

  Details: the all-gates-blocking flip's first draft silently added `Intent
  section` + `Plan-lock gate` to `branch_protection.required_contexts` — a route
  ADR-0022 and plan-lock-enforcement Q7 had EXPLICITLY REJECTED (the label hatches
  can't reach GitHub branch protection; `plan-lock-gate.yml` has no `labeled`
  re-trigger, so a red + override label = unmergeable). The code-review round
  caught it; a gate would catch the class. Proposal: a doc-validation check that,
  for each name added to `required_contexts` vs `origin/develop`, greps
  `docs/adr/*.md` + `docs/plans/shipped/*.md` for that name inside a "rejected" /
  "NOT a required" / "do not add … to branch_protection" context and FAILs (or
  WARNs) with the citation, forcing a superseding ADR note. Home: a new
  `test-required-context-adr-consistency.sh` in the doc-validation suite.
  Cross-ref: shipped/all-gates-blocking.md § Deviations; docs/adr/0022-intent-gate-promotion.md.
