- 2026-07-05 · claude-code · [tooling] · P3 — perf-compare delta table shows big % on 1-sample scopes without flagging them as below-floor noise

  Details: `scripts/dev/perf-compare.py`'s per-scenario delta table (surfaced in the
  `Perf PR-fast` job summary + PR comment) prints eye-catching relative deltas for
  scopes that have too few samples to be meaningful. On PR #1632's
  `ai-chat-history-render` run, `SmatchetUI::Draw` read `0.424 → 0.493 ms (+16.2 %)`,
  `SmatchetToolbarUi::Draw +56.9 %`, `SmatchetToastManager::Render +3575.0 %` — all
  with **`baseline calls = 1`**. The GATE correctly reports 0 regressions (the
  `min_baseline_calls = 10` floor + `mean_min_abs_delta_ms = 0.05` noise floor in
  `regression-policy.json` reject them), but the TABLE renders the raw percentages
  with no marker, so a human reading the PR sees "+3575 %" and reasonably suspects a
  real regression. This session had to hand-explain in the PR body why those aren't
  regressions — the presentation should carry that itself.

  Impact: not a gate bug (the gate is correct), but a **legibility** gap that
  produces false alarm + wasted triage on every low-sample scenario. The PR author /
  reviewer can't tell "this % is noise below the sample floor" from "this % is a real
  move" without cross-referencing the policy thresholds by hand.

  Concrete next action: in `perf-compare.py`'s table renderer, tag any row whose
  `baseline calls < min_baseline_calls` (or whose absolute delta < mean_min_abs_delta_ms)
  with an inline marker — e.g. append `· (noise: <N samples < floor)` or move such
  rows under a collapsed "below sample/noise floor — not gated" sub-section — so a
  reader distinguishes gated signal from sampling noise at a glance. Optionally sort
  gated-eligible rows first. Keep the raw numbers (transparency), just annotate.

  Cross-ref: PR #1632 Validation section (the hand-written noise explanation this
  would have made unnecessary); `docs/perf/regression-policy.json` (the floors).
