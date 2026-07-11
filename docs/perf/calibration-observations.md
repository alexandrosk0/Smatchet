# Perf-gate step-5 calibration — observed CI distribution (Phase 1)

**Date:** 2026-07-05 · **Plan:** [`docs/plans/shipped/perf-gate-step5-calibration.md`](../plans/shipped/perf-gate-step5-calibration.md)
**Method:** harvested the `perf-pr-fast-snapshots` artifact from **6 green `Perf PR-fast (windows-2022)` runs** on `develop`-based branches (2026-07-05, run IDs 28754543733 / 28755096721 / 28755729852 / 28756059521 / 28757855251 / 28758047020). Each run captures **3 repetitions × 6 scenarios** = 18 snapshots → 18 samples per scenario per scope. Raw metric = `build/perf-runs/<scenario>-<ts>.json` rows (`avgPerCallMs`, `p99Ms`, `maxMs`, `calls`).

## Headline result — the mean budget can be armed safely, with no overrides

**Zero scopes exceed the Pillar-1 6.94 ms (1000/144) mean budget** across all 6 runs. The hottest scope in every scenario is `SmatchetUI::Draw` — the *once-per-frame whole-frame umbrella* — and even it runs **13–15× under budget**:

| scenario | hottest scope | mean avg (ms) | max avg (ms) | max p99 (ms) | budget headroom |
|---|---|---:|---:|---:|---:|
| ai-chat-history-render | SmatchetUI::Draw | 0.531 | 0.564 | 0.695 | 13.1x |
| cell-edit-burst | SmatchetUI::Draw | 0.496 | 0.564 | 0.631 | 14.0x |
| concurrent-sync | SmatchetUI::Draw | 0.459 | 0.542 | 0.603 | 15.1x |
| idle | SmatchetUI::Draw | 0.454 | 0.512 | 0.589 | 15.3x |
| priority-grid-scroll | SmatchetUI::Draw | 0.469 | 0.513 | 0.857 | 14.8x |
| side-by-side-2-grid | SmatchetUI::Draw | 0.470 | 0.518 | 0.592 | 14.8x |

- **p99 headroom vs the live 10 ms ceiling:** worst observed is `priority-grid-scroll / SmatchetUI::Draw` at **0.857 ms** — 11.7× under.
- **No legitimately-heavy scope** exists that would need a `perScenario` `mean_abs_ceiling_ms` override. The Phase-1 classification (heavy-but-legit vs risk) is therefore trivial: **every scope is comfortably under budget**; `default.mean_abs_ceiling_ms = 6.94` with an empty `perScenario` map is sufficient.

## Correction to the T2 "shallow baselines" concern

The step-5 backlog entry and plan flagged that baseline `calls = 1–2` starves the relative gate. Phase-1 data **refines this**:

- Scenarios are **not** shallow — `frames = 600` (with `warmup = 30` on `ai-chat-history-render`); `cell-edit-burst` is the intentional 1-frame burst.
- `calls = 1` is a **snapshot artifact**, not a short run: the fresh snapshot reports the *last frame's* row set, and umbrella scopes fire once per frame. But `avgPerCallMs` / `p99Ms` / `maxMs` in that same row are **aggregated across the full 600-frame sample ring** — so the p99 (≤10 ms) and max (≤50 ms) absolute ceilings already gate on real 600-frame statistics, not a single frame.
- **Net:** the absolute ceilings are healthier than the backlog implied. What the `min_baseline_calls = 10` floor actually suppresses is the *relative lastTotalMs %-delta* on once-per-frame scopes — deliberately, because a single-frame `lastTotalMs` is noisy. The armed mean ceiling (below) closes the real gap; deepening baselines for the relative gate is **optional polish**, not load-bearing.

## Recommendation for Phase 3 (user-gated flip)

1. **Arm** `regression-policy.json → default.mean_abs_ceiling_ms = 6.94`, `perScenario` unchanged (empty). Evidence: 13–15× headroom on the hottest scope; no scope within 7× of the budget.
2. **No WARN-first needed on risk grounds** — but WARN-first is impossible anyway (perf-compare's mean-cap branch is binary/blocking). Given the headroom, a direct blocking arm carries negligible false-positive risk.
3. **Baseline recapture / frame-count deepening (Phase 2):** downgrade to optional follow-up — the absolute ceilings already run on 600-frame data. Recapture only if the relative %-delta gate is later wanted for multi-call scopes.

The flip itself is a live-gate tighten → **user sign-off required** per the plan + AI_POLICY § escalate-when-unvalidatable; this doc is the evidence packet for that decision.
