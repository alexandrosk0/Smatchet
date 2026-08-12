# Perf gate — step-5 calibration (arm the mean budget + deepen coverage)
<!-- plan-date: 2026-07-06 -->

**Status:** SHIPPED (2026-07-06) · **Owner:** perf-detective / build-doctor · **Created:** 2026-07-05
**Predecessor:** [`docs/plans/shipped/perf-gate-revival.md`](../shipped/perf-gate-revival.md) (steps 1–4, 6a, 6b, 7 shipped; step 5 = this plan)
**Governance:** tightening a live gate's numeric thresholds is a human-judgment call — every threshold-arming step here is **user-gated**, never an autonomous flip (AI_POLICY § Escalate when unvalidatable).

## Problem

`Perf PR-fast (windows-2022)` is a required branch-protection context and blocks via the poller (`MERGE_GATES_BLOCK_ALLOWLIST_RE="."`). Its **absolute** teeth are live and fed: p99 ≤ 10 ms and max ≤ 50 ms fire on every scope (CR-949-1), and the `p99Ms` emitter has landed. Two teeth remain retracted:

- **T1 — mean budget null.** `regression-policy.json → default.mean_abs_ceiling_ms = null`. The Pillar-1 6.94 ms (1000/144) steady-state budget is unenforced; an 8 ms `avgPerCallMs` scope passes.
- **T2 — shallow baselines.** Every `ci-windows-latest` baseline has per-scope `calls = 1–2` (1 scope of ~192 clears `min_baseline_calls = 10`). The relative 10 %-delta gate skips below-floor rows by design → effectively inert for steady-state drift that stays under the absolute ceilings. Baselines also predate the `p99Ms` emitter, so baseline rows carry no `p99Ms`.

Neither is a bug — both are the consciously-deferred "step 5" from the revival playbook (revival.md § Deviations, line 101). This plan closes them with evidence, not guesswork.

## Non-goals

- Not re-litigating steps 6a/6b/7 (shipped, verified).
- Not lowering p99/max ceilings — those are calibrated and live.
- No autonomous threshold flip: each arming lands only after N observed CI runs + user sign-off on the numbers.

## Approach — evidence first, then arm

### Phase 1 — Observe (no gate change) · owner: perf-detective
- **1a.** Collect `avgPerCallMs` + `p99Ms` + `calls` for every scope across ≥ 5 green `Perf PR-fast` runs on `develop` (harvest from existing run artifacts — no new infra). Produce `docs/perf/calibration-observations.md`: per-scope mean/p99/max distribution + which scopes legitimately exceed 6.94 ms mean (e.g. the `SmatchetUI::Draw` once-per-frame umbrella, whose per-call ≈ whole-frame time).
- **1b.** Classify each over-budget scope: *legitimately heavy* (needs a `perScenario` override) vs *real risk* (must stay under the global cap).

### Phase 2 — Deepen baselines · owner: build-doctor
- **2a.** Decide the sample-depth lever for T2. Two candidates, pick per Phase-1 data:
  - *(preferred)* raise per-scenario frame counts in `Source/Core/src/Commands/Scenarios/*` so hot scopes accrue ≥ 10 calls and the relative gate gains teeth without inflating CI wall-time past the PR-fast budget; **or**
  - accept absolute-ceilings-only as the intended contract for 1-frame scenarios (e.g. `cell-edit-burst`) and document that the relative gate is a bonus, not the primary mechanism.
  - **Perf-gate section (mandatory — this phase touches `Source/Core/`):** any frame-count change is itself perf-visible; it must ride its own `Perf PR-fast` lane and show the CI numbers before/after. No local runtime claims (this container can't run the Windows app).
- **2b.** Recapture all six `ci-windows-latest` baselines on the runner so rows carry `p99Ms` + the new call depth. **Human-approved golden baselines** (golden-image-approval.md) — never self-approve a baseline.

### Phase 3 — Arm (user-gated) · owner: perf-detective + user
- **3a.** Set `default.mean_abs_ceiling_ms = 6.94` + the Phase-1 `perScenario` overrides, in one PR whose body pastes the observed distribution justifying each number.
- **3b.** Land WARN-first if the first armed run shows any borderline scope (mirror the original `duplication` calibration precedent), graduate to blocking after 3 clean runs.
- **3c.** Update `docs/perf/regression-policy.json` description + revival.md § Deviations "open calibration follow-up" → resolved; archive this plan to `shipped/`.

## Files in play

- `docs/perf/regression-policy.json` (arm the knob + overrides)
- `docs/perf/baselines/*.ci-windows-latest.json` (recapture — human-approved)
- `Source/Core/src/Commands/Scenarios/*` (Phase-2 frame counts, *if* chosen)
- `scripts/dev/perf-compare.py` (no change expected; the mean-cap branch already exists at line ~242, gated on non-null)
- `docs/perf/calibration-observations.md` (new; Phase-1 evidence)
- `tests/bats/merge_gates.bats` (regression check if allow-list touched — not expected)

## Implementation log

- 2026-07-05 — plan created from the perf-gate health audit ("is the perf gate mandatory / okay"). Confirmed: gate required + blocking + p99/max armed + p99 emitter landed; mean budget null + baselines shallow are the residue. No code touched yet.
- 2026-07-05 — **Phase 1 COMPLETE** ([`docs/perf/calibration-observations.md`](../../perf/calibration-observations.md)). Harvested 6 green `Perf PR-fast` runs (18 samples/scenario). **Result:** zero scopes exceed the 6.94 ms mean budget — hottest is `SmatchetUI::Draw` at ~0.53 ms (13–15× headroom); worst p99 0.857 ms vs the 10 ms ceiling. Arming `mean_abs_ceiling_ms = 6.94` is safe with an **empty `perScenario` map** (no legitimately-heavy scope exists). Phase-2 baseline-deepening **downgraded to optional** — scenarios already run `frames = 600` and the p99/max ceilings gate on the full 600-frame ring; the `calls = 1` was a once-per-frame *snapshot artifact*, not a shallow run (T2 corrected).
- 2026-07-06 — **Phase 2 COMPLETE** (#1650/#1655/#1656/#1658 tooling + #1659 goldens). Added a `recapture_baselines` manual dispatch to `perf-full.yml` (4 iterations to get it right — `if:` starvation → PowerShell shell → 18-file scope → PR-create-policy; each caught only by an actual CI run) and recaptured all 6 `ci-windows-latest` baselines so every row now carries `p99Ms`. Golden-approved + merged (#1659). CR's one "Major" finding (calls=1 maxMs≠lastTotalMs) refuted as a false positive (#1261 median-override decouples the fields by design). **Side discovery** (backlogged infra/P2): `perf-full`'s issue/PR steps lack `shell: bash` + the repo blocks Actions-created PRs → the scheduled full-suite had been silently red for a week.
- 2026-07-06 — **Phase 3 COMPLETE — mean budget ARMED.** `regression-policy.json → default.mean_abs_ceiling_ms: null → 6.94`, empty `perScenario` (user-authorized). Functionally verified before ship: armed policy fires on a synthetic avg=8.0 ms scope (`exceeds Pillar 1 mean budget 6.940`, exit 1), the null-policy control does not, and baseline-vs-itself stays within policy (no false-flag). Safety sweep: all 6 committed baselines ≤ 0.72 ms avg (9.6× under budget). The arming PR itself rides `Perf PR-fast`, self-validating on a real CI run. **Plan shipped.**

## Deviations

- **Phase 2 downgraded from required to optional.** Phase-1 data showed the absolute p99/max ceilings already operate on 600-frame aggregates (not single frames), so deepening baselines is no longer load-bearing for arming the mean budget — it only matters if the *relative* %-delta gate is later wanted for once-per-frame umbrella scopes. Recorded rather than silently dropped.
- **Phase 3 arming was held for user sign-off, then authorized 2026-07-06.** Flipping a live gate's numeric threshold is a human-judgment call (plan preamble + AI_POLICY § escalate-when-unvalidatable); the user gave an explicit go after reviewing the Phase-1 evidence. Armed directly (blocking, not WARN — perf-compare's mean-cap branch is binary; the 9.6× headroom makes false-positive risk negligible).

## Verification

- **Phase 1:** `calibration-observations.md` committed; ≥ 5 runs sampled; over-budget scopes classified.
- **Phase 2:** recaptured baselines carry `p99Ms`; hot scopes clear `min_baseline_calls`; frame-count PR (if any) shows its own green `Perf PR-fast` with cited deltas.
- **Phase 3 (done):** verified locally before ship — `perf-compare.py` with the armed policy flags a synthetic avg=8.0 ms row (`SmatchetUI::Draw: avgPerCallMs 8.000 exceeds Pillar 1 mean budget 6.940`, exit 1); the null-policy control does not fire (exit 0); baseline-vs-itself stays within policy. The arming PR triggers `Perf PR-fast` (a `regression-policy.json` change is perf-relevant), self-validating the armed gate on a real CI run.
