# Drop the required human approval on `develop` for the solo workflow

**Status:** accepted

On a solo repo GitHub forbids approving your own PR, so a branch-protection
`required_approving_review_count` of 1 is an unsatisfiable deadlock — every PR
sits `mergeStateStatus: BLOCKED` even with CI + CodeRabbit fully green (observed
on #747). The harness merge model already does **not** require a human approval:
`agents/scripts/core/merge-gates.sh` treats `reviewDecision ∈ {APPROVED, NONE}`
as a pass, and CodeRabbit (hard-blocking) + the four required CI contexts are the
real gates. So we set `required_approving_review_count: 0` on `develop`,
removing a gate that is both impossible to satisfy solo and redundant with the
bot review — **not** any correctness gate (CR + CI + the user-comment gate all
remain).

## Considered options

- **Keep 1 required review** — rejected: unsatisfiable solo; forces an admin
  merge on every PR (`enforce_admins: false` already lets the maintainer bypass,
  so the "1 review" provided essentially no real protection — only friction).
  *(Amendment 2026-08-16, merge-pipeline-06: `enforce_admins` has since flipped
  to `true` — the admin bypass this rationale references no longer exists
  server-side; the sanctioned admin path is `safe-admin-merge.sh`, which only
  merges when the full gate set is green. The decision this ADR records —
  review-count 0 — stands on its remaining grounds.)*
- **A second identity to self-approve** — rejected: none exists; a sock-puppet
  approver is worse than no gate.
- **Review-count 0 + codify (chosen)** — aligns GitHub with the harness's
  existing merge model and records the desired state in
  `project.config.json` § `branch_protection`, applied by the idempotent
  `agents/scripts/core/setup-branch-protection.sh` so it can't drift silently.

## Revisit trigger

If the repo ever takes **outside contributions**, the "solo, trusted author"
premise no longer holds — re-introduce review enforcement for non-maintainer
PRs (a ruleset or CODEOWNERS bypass list that requires review from everyone
except the maintainer). Until then, 0 is correct.
