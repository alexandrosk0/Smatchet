- 2026-11-19 · input-ux-w2-action-keys · [debt] · P2 — Archive the `input-ux-w2-action-keys` plan (move to `shipped/` and add the INDEX row) once `docs/plans/INDEX.md` is free

Details:
PR #2443 (Wave 2 piece 4.4) left its plan under `docs/plans/active/` although the Implementation log, Deviations and
Verification (actual) are filled in. Plan-lock `pane-picker-jira-sites` (branch `claude/plus-dropdown-two-jira-261a42`)
holds `docs/plans/INDEX.md`, and both the pre-push gate and the CI `Plan-lock gate` refuse a diff that touches it, so
the index row and the `git mv` were taken out of the PR (owner instruction) instead of overriding the lock.

Concrete next action:
After #2443 merges and the lock is released: in a docs-only change, flip the plan's Status to `shipped`,
`git mv` it from `docs/plans/active/` into `docs/plans/shipped/` (same basename), delete its
`## Archive` block, run `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`, and ship it as a pure-docs PR.

Status: open
Last-reviewed: 2026-11-19
