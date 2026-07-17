# Verifier sidecar

Plan: [`../plans/llm-verifier-sidecar.md`](../plans/llm-verifier-sidecar.md).

The agentic layer can use an LLM-as-a-Verifier pattern, but only as an **advisory signal** until it is calibrated. The verifier scores agent outputs, candidate plans, review findings, or fixes; deterministic gates still own merge authority.

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
| `project_invariants` | Respects the project's rules such as C++14, gates, plans, and quality pillars. |
| `scope_discipline` | Avoids unrelated rewrites and authority expansion. |
| `verification_completeness` | Validation is proportional to risk and has no silent manual residue. |

## Fine-grained reward

A single discrete label throws away the model's own uncertainty. When the backend exposes token-level logprobs, feed the distribution over the score tokens (`A`=best … `T`=worst, or `1`–`20`) instead of a scalar. The helper takes the **expectation** over that distribution, normalises it to `[0,1]`, and additionally reports:

- `variance` — spread of the score distribution, a per-sample noise estimate;
- `valid_mass` — probability the model kept on real score tokens (low mass means it hedged onto non-score tokens and the sample is malformed).

Both fold into the aggregate `uncertainty`. Scalars are still accepted for backends without logprobs; the two mix freely within one candidate.

## Producing the logprobs — `verifier-produce.py`

`scripts/dev/verifier-produce.py` is the **model-calling half**: it drives a real LLM and emits exactly the JSON `verifier-sidecar.py aggregate` consumes. It is pure-stdlib (`urllib`, no SDK) and provider-agnostic through an OpenAI-compatible `/chat/completions` endpoint.

The technique is **constrained single-token scoring**: for each criterion it asks the model to answer with exactly one letter `A`(best)…`T`(worst), then reads `top_logprobs` at that one position. A confident model concentrates mass on one letter; a torn model spreads it — and that spread becomes the sidecar's `variance`/`uncertainty`. One criterion = one call = one rubric, so the logprobs stay interpretable; the candidate text is framed as untrusted evidence, never instructions.

Two modes:

- `logprobs` (default) — needs a backend that returns token logprobs (**vLLM / SGLang / OpenAI-compatible / Vertex-Gemini**). Emits `{"logprobs": {...}}` per criterion.
- `scalar` — the fallback for backends **without** logprobs (e.g. the Anthropic Messages API today): parse the single emitted letter/number into a point score.

Config via `VERIFIER_BASE_URL` / `VERIFIER_MODEL` / `VERIFIER_API_KEY` (or flags). Repeated evaluations run the first sample at temperature 0 (the mode) and resamples hotter so `K` draws actually explore. Because calls scale as candidates × criteria × repeats, use the sidecar's pivot-tournament schedule to spend the repeat budget only on the pivots. Pipe the two halves together:

```
python scripts/dev/verifier-produce.py job.json | python scripts/dev/verifier-sidecar.py aggregate -
```

`--responses <file>` replays recorded chat-completion bodies in call order, so the producer is testable with **no network** (the bats suite and `--selftest` use this). `--record <file>` does the reverse on a live run: it tees every model response to a `--responses`-compatible file, so a real run captures a replayable trace for offline re-runs and for calibration.

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

## Calibration — advisory vs blocking

`scripts/dev/verifier-calibrate.py` decides whether the verifier is trustworthy enough to promote from advisory to blocking, the same judge-vs-human posture as `agent-eval-calibrate.py` but with the metrics that fit a probabilistic forecaster. It reads a calibration set that pairs each verifier `overall_score` with a ground-truth `outcome` (1 = the candidate really was good/correct/safe, 0 = not) and reports:

- **Brier score** — `mean((score − outcome)²)`, the proper score for a probability forecast;
- **ECE** (Expected Calibration Error) — bin the scores; per bin, the gap between mean predicted score and the empirical outcome rate. "When it says 0.8, does it happen 80% of the time?";
- **AUC** — does it rank real-good above real-bad at all?;
- **hard-veto precision / recall** — do vetoes land on genuinely bad candidates, and catch them?

Promotion is gated on all of `min_samples` / `max_brier` / `max_ece` / `min_auc`; until every threshold holds the report says `stay-advisory`. It stays a **report** (exit 0) — promotion is a human decision it informs; `--gate` makes "not yet eligible" a hard exit 1 for a CI job guarding a future blocking switch.

The end-to-end loop:

```sh
python scripts/dev/verifier-produce.py job.json --record trace.json \
  | python scripts/dev/verifier-sidecar.py aggregate - > scores.json
# label the candidates by ground-truth outcome, then:
python scripts/dev/verifier-calibrate.py --scores scores.json --labels labels.json
```

`trace.json` is the replayable evidence of that run; accumulate traces + labels and the calibration report is what justifies flipping any part of the verifier from advisory to blocking.

### Smoke-testing the live path

`scripts/dev/verifier-endpoint.py` is a dependency-free, OpenAI-compatible `/chat/completions` server that serves **deterministic canned responses**, so the whole loop can run over real HTTP with no model, API key, or GPU:

```sh
python scripts/dev/verifier-endpoint.py --port 8900 &
VERIFIER_BASE_URL=http://127.0.0.1:8900 \
  python scripts/dev/verifier-produce.py job.json --model stub \
  | python scripts/dev/verifier-sidecar.py aggregate -
```

Each answer is a hash of the request shaped into a plausible single-token A-T distribution, so it exercises both producer modes (logprobs and scalar) and gives the sidecar real spread — but it is **not a model** and its scores carry no judgement, so it proves the *wiring*, not calibration quality. `--fixed A` pins every answer (handy for a known-score smoke test); with no `--port` it binds an ephemeral port and prints `endpoint: http://…` for a caller to capture. For the fine-grained logprobs path against a real self-hosted model, point `VERIFIER_BASE_URL` at a logprob-capable backend (vLLM / SGLang) instead.

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
