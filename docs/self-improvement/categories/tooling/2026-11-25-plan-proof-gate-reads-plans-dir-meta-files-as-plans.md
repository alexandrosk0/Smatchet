# The plan CI-proof gate reads the `_*.md` meta-files in `docs/plans/active/` as plans

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-25
- **Observed on**: independent review of the agent-layer bump 75e1728 -> 4334c2b (the-unwilling-agentic-bunch#23, the plan CI-proof gate). Latent: nothing has fired yet.
- **Status**: open

## What happened

`_mg_planproof_is_plan` in `agent-layer/agents/scripts/core/merge-gates.d/30-plan-proof.sh` accepts any `docs/plans/active/*.md` or `docs/plans/shipped/*.md` one level deep. That glob also matches the three meta-files in `docs/plans/active/`, which are not plans:

- `_plan-template.md` has a real `## Verification` heading (line 99).
- `_plan-locks.generated.md` is bot-rendered.
- `_plan-locks-archive.md` is the archive of released locks.

Placeholder names such as `<check>` are already filtered out. But a docs-only edit to the template that adds a real check name as an example (`CI: "Sanitizer (UBSan via Clang)" green on this PR`) would be read as a proof. A docs-only PR skips that lane, so the gate would block the merge with `BLOCK: plan CI-proof skipped …`.

The host's pre-push predictor, `scripts/dev/plan-ci-proof-check.sh`, has the same scope. Its pathspecs (lines 141-143) are `docs/plans/active/*.md` and `docs/plans/shipped/*.md`, which also match `_*.md`, so it would predict the same false skip.

## Concrete next action

Make both copies skip a basename that starts with `_`:

- In the layer, add `docs/plans/active/_*|docs/plans/shipped/_*) return 1 ;;` ahead of the accepting arm in `_mg_planproof_is_plan`. Add a fixture to `agent-layer/tests/bats/merge_gates.bats` where an added `CI:` line in `_plan-template.md` is ignored.
- In the host, add the exclude pathspecs `':(exclude)docs/plans/active/_*'` and `':(exclude)docs/plans/shipped/_*'` to the three `git diff` / `git ls-files` calls in `plan-ci-proof-check.sh`.

The fix in [`2026-11-25-plan-proof-parser-hand-copied-host-vs-layer.md`](2026-11-25-plan-proof-parser-hand-copied-host-vs-layer.md) would let the host take the layer's `_mg_planproof_is_plan` and fix both at once.
