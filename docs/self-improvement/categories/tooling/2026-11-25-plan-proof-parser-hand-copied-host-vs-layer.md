# The plan CI-proof parser is hand-copied between the host's pre-push check and the layer's merge gate

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-25
- **Observed on**: independent review of the agent-layer bump 75e1728 -> 4334c2b (the-unwilling-agentic-bunch#23, the plan CI-proof gate).
- **Status**: open

## What happened

Two scripts decide which `CI: "<check>" green on this PR` lines in a plan's `## Verification` section count as proofs:

- the host's pre-push predictor, `scripts/dev/plan-ci-proof-check.sh` (parser around lines 85-110);
- the layer's merge-time gate, `agent-layer/agents/scripts/core/merge-gates.d/30-plan-proof.sh` (`_mg_planproof_checks`, around line 98).

The parser is the same code in both: the fence-tracking awk, the `## Verification` section awk with its added-line filter, and the `grep -oE` / `sed -E` / `grep -vE '^<[^>]*>$'` pipeline. The host copy carries a comment saying it is kept in step with the layer's, but nothing checks that. No test in the host or the layer runs both copies on the same fixtures. A search of `tests/`, `scripts/` and `.github/` for `30-plan-proof` and `_mg_planproof` finds no shared test.

If they drift, pre-ship predicts one verdict and the merge gate applies another. The point of the pre-push check is to catch at push time the skip the gate would block on at merge.

## Concrete next action

Have the host source the layer module instead of keeping its own copy. `plan-ci-proof-check.sh` sources `"$AGENT_LAYER_ROOT/agents/scripts/core/merge-gates.d/30-plan-proof.sh"`, which only defines functions and is safe to source, then calls `_mg_planproof_checks <file> <added>` and `_mg_planproof_is_plan`. Delete the host's copy of the awk/grep pipeline.

Resolve `AGENT_LAYER_ROOT` the way the other host wrappers do, through `scripts/dev/project-config.sh`, and fail closed when the mount is missing. Keep the existing host bats cases for `plan-ci-proof-check.sh`. They become the cross-copy test, because they now run the layer's parser.

Doing this also lets the `_*.md` fix in [`2026-11-25-plan-proof-gate-reads-plans-dir-meta-files-as-plans.md`](2026-11-25-plan-proof-gate-reads-plans-dir-meta-files-as-plans.md) land in one place.
