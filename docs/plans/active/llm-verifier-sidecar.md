# Plan — LLM verifier sidecar

> **Slug**: `llm-verifier-sidecar`
>
> **Status**: `active`

## Context

The maintainer asked to apply the paper “LLM-as-a-Verifier: A General-Purpose Verification Framework” to Smatchet. The useful seam is the agentic governance layer: Smatchet already has deterministic merge gates and an advisory agent-eval harness, but `pre-merge-review` collapses reviewer output through one categorical judge result.

After this lands, pre-merge review exposes continuous verifier scores, uncertainty, and hard vetoes alongside the existing merge recommendation, and the repo has a deterministic helper that re-derives the paper's core machinery — fine-grained logprob reward, repeated-evaluation uncertainty, Bradley-Terry ranking, a probabilistic pivot tournament, and progress tracking — as a pure-stdlib, provider-agnostic aggregator.

## Approach

Add the verifier as a sidecar, not a new merge authority. The workflow judge emits criterion scores and a hard-veto flag; a pure-stdlib helper aggregates repeated samples, rankings, and progress curves offline. The helper is a from-scratch reimplementation inspired by the paper (not a port): it treats verifier output as data and never calls a model. Documentation binds the operating rule: deterministic gates still own merge decisions until calibration data justifies promotion.

## Files to modify

1. `agents/_shared/workflows/pre-merge-review.js` — extend the judge schema and prompt with continuous verifier output while keeping `merge_recommendation`.
2. `scripts/dev/verifier-sidecar.py` — deterministic helper: fine-grained logprob reward (expectation over the A–T score scale, with distribution variance + valid-token mass), repeated-evaluation aggregation with a standard error and confidence interval, SEM-driven adaptive resampling, hard-veto propagation, Bradley-Terry MLE ranking, a full pivot-tournament schedule, and progress tracking. Modes: `aggregate` / `reward` / `track` / `--selftest`.
3. `docs/agent-rules/verifier-sidecar.md` — operating contract and calibration posture.
4. `docs/agent-rules/subagent-eval.md` — link the verifier sidecar to the existing advisory eval harness.
5. `docs/agent-rules/workflow-orchestration.md` — update the workflow summary.
6. `docs/plans/active/llm-verifier-sidecar.md` — this plan.
7. `tests/bats/verifier_sidecar.bats` + `scripts/dev/test-verifier-sidecar-bats.sh` — Bucket A coverage (fine-grained reward, veto→block+ranking, O(Nk) pivot budget, progress early-stop, exit-code contract), auto-enrolled by `test-all.sh` and required by the orphan-bats gate.

## Existing utilities reused

- `agent-eval-score.py` policy: reused conceptually for normalized `[0,1]` scores and advisory-before-calibrated posture.
- `agent-eval-calibrate.py` policy: reused conceptually for judge-vs-human graduation before blocking.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this adds a small helper and a focused doc; it does not extract an over-cap source.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no product runtime impact; agent-only docs/scripts.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no product runtime impact.
- **Pillar 3 (never crash)**: no product runtime impact.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no UI change.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — the diff does not touch `Source/Core/`.

## Risks / non-goals

- **Risk: verifier becomes misplaced trust.** Mitigation: docs and schema make it advisory and preserve hard deterministic gates.
- **Risk: false precision from one sample.** Mitigation: helper reports uncertainty and recommends repeated evaluations near thresholds.
- **Non-goal: live model integration.** The helper aggregates model outputs but does not call a provider.
- **Non-goal: merge-gate promotion.** Blocking requires calibration and a follow-up change.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ test-rig code.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no UI code.
- **Bash-driver scenario / screenshot / sanitizer**: `python scripts/dev/verifier-sidecar.py --selftest` plus the bats suite `bash scripts/dev/test-verifier-sidecar-bats.sh` (7 cases, run in CI by the agentic-selftests lane via `test-all.sh`).
- **Build gate**: skipped — pure docs plus Python helper; no product compile path.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: run `bash scripts/dev/test-docs.sh` if available in the environment.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: self-reviewed against `AGENTS.md`, `subagent-eval.md`, and `workflow-orchestration.md`; the key constraint is advisory-only until calibrated.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

- ~~Live provider integration~~ — **landed** as `scripts/dev/verifier-produce.py`: drives an OpenAI-compatible endpoint with constrained single-token A-T scoring, extracts `top_logprobs`, and emits the sidecar's aggregate input (scalar-mode fallback for logprob-less backends). Replay-transport tested (no network). Remaining follow-up: point it at a live self-hosted verifier model and record calibration traces.
- Executing the pivot-tournament schedule inside workflows — the helper *emits* the ring + pivot comparison schedule; a workflow that runs those comparisons and closes the loop is a follow-up.
- Dense RL rewards for autonomous fix loops — not appropriate before calibration and trace coverage.

## Implementation log

*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan

*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)

*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)

*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
