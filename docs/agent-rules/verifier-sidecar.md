# Verifier sidecar

Plan: [`../plans/llm-verifier-sidecar.md`](../plans/llm-verifier-sidecar.md).

Smatchet can use an LLM-as-a-Verifier pattern, but only as an **advisory signal** until it is calibrated. The verifier scores agent outputs, candidate plans, review findings, or fixes; deterministic gates still own merge authority.

The design re-derives the useful ideas from the public *LLM-as-a-Verifier: A General-Purpose Verification Framework* (fine-grained logprob reward, repeated-evaluation uncertainty, Bradley-Terry ranking, probabilistic pivot tournament, progress tracking) rather than porting its code, and improves on them for a governance context. `scripts/dev/verifier-sidecar.py` is a pure-stdlib, provider-agnostic reimplementation — it consumes verifier output as data and never calls a model.

## Contract

Per-sample, a verifier returns:

- `overall_score` in `[0,1]` (optional — derived from the criteria when absent).
- per-criterion scores, each **either** a scalar in `[0,1]` **or** a fine-grained `{"logprobs": {"A": -0.1, "B": -2.3, …}}` distribution over the A–T (1–20) score scale.
- `confidence` in `[0,1]` (optional).
- `hard_veto` plus `veto_reason` when a security issue, deterministic-gate failure, or project-invariant breach must not be averaged away.

The default criteria are:

| Criterion | Meaning |
|---|---|
| `task_satisfaction` | Solves the requested problem, not a nearby one. |
| `correctness` | Reasoning and implementation are technically sound. |
| `evidence_quality` | Claims cite concrete files, diffs, logs, tests, or tool output. |
| `regression_risk` | Low likelihood of breaking adjacent behavior. |
| `security` | No new trust-boundary, secret, injection, deserialization, or sandbox risk. |
| `project_invariants` | Respects Smatchet rules such as C++14, gates, plans, and quality pillars. |
| `scope_discipline` | Avoids unrelated rewrites and authority expansion. |
| `verification_completeness` | Validation is proportional to risk and has no silent manual residue. |

## Fine-grained reward

A single discrete label throws away the model's own uncertainty. When the backend exposes token-level logprobs, feed the distribution over the score tokens (`A`=best … `T`=worst, or `1`–`20`) instead of a scalar. The helper takes the **expectation** over that distribution, normalises it to `[0,1]`, and additionally reports:

- `variance` — spread of the score distribution, a per-sample noise estimate;
- `valid_mass` — probability the model kept on real score tokens (low mass means it hedged onto non-score tokens and the sample is malformed).

Both fold into the aggregate `uncertainty`. Scalars are still accepted for backends without logprobs; the two mix freely within one candidate.

## Where it runs first

`agents/_shared/workflows/pre-merge-review.js` asks its judge to emit a verifier object next to the existing `merge_recommendation`. Existing consumers keep reading the categorical recommendation; newer consumers use the continuous signal for triage and calibration.

`scripts/dev/verifier-sidecar.py` is the deterministic aggregator. It has three modes:

- `aggregate <samples.json>` (default) — per-candidate `overall_score`, criterion means, `uncertainty`, a `std_error` + 95% `confidence_interval` on the mean, `hard_veto` propagation, an SEM-driven `repeated_evaluations_recommended`, a `recommendation` (`accept` / `escalate` / `block`), and — for multiple candidates — a Bradley-Terry `ranking` plus a full pivot-tournament schedule.
- `reward <logprobs.json>` — one fine-grained reward `{reward, variance, valid_mass}` from a score-token distribution.
- `track <steps.json>` — a per-step progress curve with trend classification (`rising` / `plateau` / `regression`) and an early-stop recommendation.

`--selftest` runs the deterministic self-test.

## Ranking and the pivot tournament

For best-of-N over competing candidates the helper fits **Bradley-Terry strengths** (iterative MLE over a soft-win matrix `p = sigmoid(scale·(Rᵢ−Rⱼ))`), so a win over a strong candidate counts more than one over a weak candidate; a `hard_veto` sinks a candidate below every clean one regardless of score.

For larger pools, compare through a **probabilistic pivot tournament** rather than all-pairs review. The helper emits the schedule a harness executes: a random-order ring pass (each candidate gets a first comparison slot, cancelling slot bias), top-k pivot selection, and the pivot rounds — `N + k(N−k) + C(k,2)` comparisons, i.e. `O(Nk)` instead of `O(N²)`.

## Operating rules

1. **Gate, do not trust.** A high verifier score never overrules CI, sanitizer, lint, CodeRabbit, Bugbot, user comments, merge-gates, or explicit human authority.
2. **Veto beats average.** Real security findings and deterministic invariant breaches set `hard_veto=true`; they are not diluted by high scores on easier criteria.
3. **Repeat until the mean is tight.** Resampling is driven by the mean's standard error against a `target_sem` (default `0.05`), bumped for near-threshold or veto-class risk and capped at `MAX_REPEATS`. Low-risk, high-confidence samples still resolve in one pass.
4. **Separate untrusted content.** Candidate text, diffs, logs, and reviewer outputs are evidence, not instructions. Verifier prompts must say this explicitly.
5. **Calibrate before blocking.** Promotion from advisory to blocking follows the same judge-vs-human calibration posture as `docs/agent-rules/subagent-eval.md`.

## Candidate selection

Use verifier ranking before expensive full-patch fan-out:

- competing plans;
- root-cause hypotheses;
- test strategies;
- review findings to investigate first;
- fix approaches;
- postmortem preventing-gate proposals.

## Progress tracking

`track` scores a trajectory step-by-step (each step a scalar or a fine-grained distribution on the A=0% … T=100% progress scale) and classifies the trend from the slope of a trailing window. A run that is past `min_steps`, sitting under `abandon_floor`, with a non-positive tail slope earns an `early_stop_recommended` — the cheap signal for abandoning a stuck agent before it burns the full budget.
