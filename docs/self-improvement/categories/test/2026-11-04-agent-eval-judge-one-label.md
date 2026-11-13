# The code-review judge's calibration set holds one human label, so judge deltas under about 0.1 are not evidence

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-04
- **Observed on**: the code-review eval of the pre-#18 prompt against the thinned one (the-unwilling-agentic-bunch#18 → Smatchet#2385).
- **Status**: open

## What happened

`docs/agent-eval/calibration/code-review/labels.json` has one entry: the dpapi strong run, with a human `correctness` label of 0.95. Whether or not `scripts/dev/agent-eval-calibrate.py` passes on that entry, one high-scoring label cannot show whether the judge separates a partial review from a miss. Re-scoring the same outputs moved the judge by about ±0.02. Severity-mismatch gaps (Critical against High) moved it by 0.05–0.07. The eval had to fall back on blind two-grader detection to call any result.

## Concrete next action

Add at least six labelled entries, one miss, one partial and one hit per case where possible. The eval's recorded outputs are suitable sources: a catalog miss (correctness ≈ 0), a catalog hit at Low instead of High, a dpapi demotion to info, an env-cred merged-bullet hit, and a clean recentlru run. Pin each one's result JSON under `docs/agent-eval/calibration/code-review/`. Until then, `agent-eval-score.py` should print a note that judge deltas under 0.1 need a detection check behind them.
