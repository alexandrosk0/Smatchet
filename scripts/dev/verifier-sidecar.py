#!/usr/bin/env python3
# scripts/dev/verifier-sidecar.py — deterministic aggregator for the
# LLM-as-a-Verifier pattern (advisory signal; never a merge authority).
#
# This is the DETERMINISTIC half of the pattern: it does not call a model. A
# harness records verifier output as JSON, then this helper turns it into
# continuous rewards, uncertainty, hard-veto status, best-of-N rankings, and
# progress curves. Pure Python 3 stdlib — no third-party deps, provider-agnostic.
# Plan: docs/plans/llm-verifier-sidecar.md.
#
# It is a from-scratch reimplementation INSPIRED BY the public
# "LLM-as-a-Verifier: A General-Purpose Verification Framework"
# (github.com/llm-as-a-verifier/llm-as-a-verifier), re-deriving the useful ideas
# rather than porting the code, and improving on them for a governance context:
#
#   * Fine-grained reward by EXPECTATION over a score-token logprob distribution
#     (a 1..G / A..T scale), normalised to [0,1] — not a single discrete label
#     (paper's fine_grained_reward). IMPROVEMENT: also returns the reward-
#     distribution variance and the valid-token probability mass, giving a
#     principled per-sample uncertainty + malformedness signal the paper drops.
#   * Multi-criteria decomposition with repeated evaluations, aggregated with a
#     standard error and a normal-approx confidence interval (the paper averages;
#     IMPROVEMENT: SEM-driven adaptive resampling instead of ad-hoc score bands).
#   * Bradley-Terry ranking. IMPROVEMENT: proper iterative MLE (MM / Zermelo)
#     strengths from the soft-win matrix, not the paper's single-pass w_i/c_i.
#   * Full Probabilistic Pivot Tournament SCHEDULE (ring pass -> top-k pivots ->
#     pivot rounds), O(Nk) — the schedule a harness executes (paper's PPT).
#   * Progress tracking with trend classification + early-stop advice (paper's
#     progress / ProgressTracker).
#   * Hard-veto propagation: security / deterministic-gate / invariant breaches
#     are NOT averaged away by good scores elsewhere (governance addition).
#
# Usage:
#   python scripts/dev/verifier-sidecar.py samples.json          # aggregate (default)
#   python scripts/dev/verifier-sidecar.py aggregate samples.json
#   python scripts/dev/verifier-sidecar.py reward logprobs.json  # one fine-grained reward
#   python scripts/dev/verifier-sidecar.py track steps.json      # progress curve
#   python scripts/dev/verifier-sidecar.py --selftest
#
# selftest: asserts-failure
#
# Aggregate input shape (a criterion score is a scalar in [0,1] OR a
# {"logprobs": {"A": -0.1, "B": -2.3, ...}} distribution over score tokens):
#   {
#     "target_sem": 0.05,                 # optional; adaptive-resampling target
#     "weights": {"security": 1.6, ...},  # optional; default DEFAULT_WEIGHTS
#     "candidates": [
#       {
#         "id": "plan-a",
#         "samples": [
#           {
#             "criteria": {"correctness": 0.8, "security": {"logprobs": {...}}},
#             "overall_score": 0.82,       # optional; else weighted from criteria
#             "confidence": 0.75,          # optional
#             "hard_veto": false,          # optional
#             "veto_reason": "..."         # optional
#           }
#         ]
#       }
#     ]
#   }
#   # Single-candidate shorthand:  {"samples": [ ... ]}
#
# reward input:   {"logprobs": {"A": -0.05, "B": -3.0, ...}}  (or {"score": 0.9})
# track  input:   {"steps": [0.1, 0.3, {"logprobs": {...}}, 0.8],
#                  "min_steps": 4, "abandon_floor": 0.1}
#
# Exit codes:
#   0 — success
#   2 — malformed input / usage error / failed self-test

from __future__ import annotations

import argparse
import io
import json
import math
import random
import sys
from typing import Any, Dict, List, Optional, Sequence, Tuple

if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )


# --- constants -------------------------------------------------------------

# Score-token scale: A..T == 20 buckets, A best (value G), T worst (value 1),
# matching the granularity-20 letter scale the verifier paper extracts logprobs
# over. Numeric tokens "1".."20" are accepted too (1 worst, 20 best).
GRANULARITY = 20
_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[:GRANULARITY]
# letter value: A -> G (best), T -> 1 (worst).
_LETTER_VALUE: Dict[str, float] = {ch: float(GRANULARITY - i) for i, ch in enumerate(_LETTERS)}
# progress mapping is the same scale read the other way: A -> 0.0, T -> 1.0.
_LETTER_PROGRESS: Dict[str, float] = {
    ch: (i / (GRANULARITY - 1)) for i, ch in enumerate(_LETTERS)
}

# Criterion weights. The paper weights criteria uniformly; we default to a
# governance-tuned profile (security / correctness / invariants dominate) but any
# input may override via "weights". Uniform is recoverable with equal values.
DEFAULT_WEIGHTS: Dict[str, float] = {
    "task_satisfaction": 1.0,
    "correctness": 1.5,
    "evidence_quality": 1.0,
    "regression_risk": 1.2,
    "security": 1.6,
    "project_invariants": 1.4,
    "scope_discipline": 0.8,
    "verification_completeness": 1.0,
}

# Aggregation / resampling policy knobs.
DEFAULT_TARGET_SEM = 0.05     # stop resampling once the mean's SEM is this tight.
MAX_REPEATS = 8               # cap on recommended repeated evaluations (paper default N).
ACCEPT_FLOOR = 0.75           # overall below this can't be a clean accept.
BLOCK_FLOOR = 0.55            # overall below this blocks outright.
BT_SCALE = 5.0                # logistic spread for sigmoid(scale*(Ra-Rb)) soft wins.


class VerifierError(ValueError):
    """Malformed verifier input."""


# --- io / numeric helpers --------------------------------------------------

def load_json(path: str) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        raise VerifierError(f"file not found: {path}")
    except json.JSONDecodeError as exc:
        raise VerifierError(f"malformed JSON in {path}: {exc}")


def clamp01(value: Any, field: str) -> float:
    try:
        v = float(value)
    except (TypeError, ValueError):
        raise VerifierError(f"{field} must be numeric")
    if math.isnan(v) or math.isinf(v):
        raise VerifierError(f"{field} must be finite")
    return max(0.0, min(1.0, v))


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def pvariance(values: Sequence[float]) -> float:
    if len(values) <= 1:
        return 0.0
    m = mean(values)
    return sum((x - m) ** 2 for x in values) / len(values)


def pstdev(values: Sequence[float]) -> float:
    return math.sqrt(pvariance(values))


def sigmoid(x: float) -> float:
    # Numerically stable logistic.
    if x >= 0:
        z = math.exp(-x)
        return 1.0 / (1.0 + z)
    z = math.exp(x)
    return z / (1.0 + z)


# --- fine-grained reward (paper core) --------------------------------------

def _valid_token_value(token: str, mapping: Dict[str, float]) -> Optional[float]:
    t = str(token).strip().upper()
    if t in mapping:
        return mapping[t]
    # Numeric score tokens "1".."G": map to the same 1..G value axis.
    if t.isdigit():
        n = int(t)
        if 1 <= n <= GRANULARITY:
            return float(n)
    return None


def fine_grained_reward(
    logprobs: Dict[str, float], mapping: Dict[str, float] = _LETTER_VALUE
) -> Dict[str, float]:
    """Expectation of a score over a token logprob distribution, normalised to
    [0,1]. Returns {reward, variance, valid_mass}.

    reward     = E[value] over score tokens, mapped from [1,G] to [0,1].
    variance   = Var of that same distribution in [0,1] units (per-sample noise).
    valid_mass = probability the model put on VALID score tokens (a malformedness
                 signal: low mass ==> the verifier hedged onto non-score tokens).

    Improvement over the paper, which takes only the expectation: the variance
    and valid_mass feed the uncertainty channel directly.
    """
    if not isinstance(logprobs, dict) or not logprobs:
        raise VerifierError("logprobs must be a non-empty object")

    # exp() the logprobs, keep only valid score tokens, renormalise over them.
    weighted: List[Tuple[float, float]] = []  # (prob, normalised_value in [0,1])
    valid_prob = 0.0
    total_prob = 0.0
    lo, hi = 1.0, float(GRANULARITY)
    for token, lp in logprobs.items():
        try:
            p = math.exp(float(lp))
        except (TypeError, ValueError, OverflowError):
            raise VerifierError(f"logprob for {token!r} is not a finite number")
        total_prob += p
        v = _valid_token_value(token, mapping)
        if v is None:
            continue
        valid_prob += p
        weighted.append((p, (v - lo) / (hi - lo)))

    if valid_prob <= 0.0:
        raise VerifierError("no valid score tokens in logprob distribution")

    reward = sum(p * nv for p, nv in weighted) / valid_prob
    var = sum(p * (nv - reward) ** 2 for p, nv in weighted) / valid_prob
    valid_mass = valid_prob / total_prob if total_prob > 0 else 0.0
    return {"reward": reward, "variance": var, "valid_mass": valid_mass}


def score_of(
    value: Any, field: str, mapping: Dict[str, float] = _LETTER_VALUE
) -> Tuple[float, float]:
    """Reduce a criterion/step value to (reward, within_sample_variance).

    Accepts a scalar in [0,1], a {"logprobs": {...}} distribution, or a single
    letter/number token string.
    """
    if isinstance(value, dict) and "logprobs" in value:
        fg = fine_grained_reward(value["logprobs"], mapping)
        # A verifier that spread mass onto non-score tokens is inflating its own
        # uncertainty; fold the missing valid_mass into the variance channel.
        malformed_penalty = (1.0 - fg["valid_mass"]) * 0.25
        return fg["reward"], fg["variance"] + malformed_penalty
    if isinstance(value, str):
        v = _valid_token_value(value, mapping)
        if v is None:
            raise VerifierError(f"{field}: {value!r} is not a valid score token")
        return (v - 1.0) / (GRANULARITY - 1.0), 0.0
    return clamp01(value, field), 0.0


# --- multi-criteria + repeated-evaluation aggregation ----------------------

def sample_overall(
    sample: Dict[str, Any], weights: Dict[str, float]
) -> Tuple[float, float]:
    """(overall reward, within-sample variance) for one repeated verification."""
    if "overall_score" in sample:
        return clamp01(sample["overall_score"], "overall_score"), 0.0
    criteria = sample.get("criteria")
    if not isinstance(criteria, dict) or not criteria:
        raise VerifierError("sample needs overall_score or criteria")
    total_w = 0.0
    weighted = 0.0
    var_acc = 0.0
    for key, value in criteria.items():
        w = float(weights.get(str(key), 1.0))
        r, var = score_of(value, f"criteria.{key}")
        total_w += w
        weighted += w * r
        var_acc += (w ** 2) * var
    if total_w <= 0:
        return 0.0, 0.0
    return weighted / total_w, var_acc / (total_w ** 2)


def recommend_repeats(
    current_k: int,
    sem: float,
    uncertainty: float,
    hard_veto: bool,
    near_threshold: bool,
    target_sem: float,
) -> int:
    """SEM-driven adaptive resampling: how many total evaluations to target.

    Improvement over a fixed 1/3/5 ladder: estimate the n needed to pull the
    mean's standard error under `target_sem` (SEM ~ 1/sqrt(n)), then clamp to
    [current_k, MAX_REPEATS] and bump for veto-class or near-threshold risk.
    """
    per_sample_sigma = sem * math.sqrt(max(current_k, 1))
    if target_sem > 0 and per_sample_sigma > 0:
        needed = math.ceil((per_sample_sigma / target_sem) ** 2)
    else:
        needed = current_k
    needed = max(needed, current_k)
    if near_threshold or uncertainty >= 0.20:
        needed = max(needed, 3)
    if hard_veto or uncertainty >= 0.30:
        needed = max(needed, 5)
    return min(max(needed, 1), MAX_REPEATS)


def aggregate_candidate(
    candidate: Dict[str, Any], weights: Dict[str, float], target_sem: float
) -> Dict[str, Any]:
    samples = candidate["samples"]
    criteria_values: Dict[str, List[float]] = {}
    overall_values: List[float] = []
    within_var: List[float] = []
    confidence_values: List[float] = []
    veto_reasons: List[str] = []
    hard_veto = False

    for index, raw in enumerate(samples):
        if not isinstance(raw, dict):
            raise VerifierError(f"{candidate['id']} sample {index} is not an object")
        criteria = raw.get("criteria", {})
        if criteria is not None and not isinstance(criteria, dict):
            raise VerifierError(f"{candidate['id']} sample {index} criteria is not an object")
        for key, value in (criteria or {}).items():
            r, _ = score_of(value, f"criteria.{key}")
            criteria_values.setdefault(str(key), []).append(r)
        overall, var = sample_overall(raw, weights)
        overall_values.append(overall)
        within_var.append(var)
        if "confidence" in raw:
            confidence_values.append(clamp01(raw["confidence"], "confidence"))
        if bool(raw.get("hard_veto")):
            hard_veto = True
            reason = raw.get("veto_reason")
            if isinstance(reason, str) and reason.strip():
                veto_reasons.append(reason.strip())

    k = len(overall_values)
    overall = mean(overall_values)
    between_var = pvariance(overall_values)          # disagreement across repeats
    mean_within_var = mean(within_var)               # in-token distribution spread
    # Total variance of one draw = between-repeat + mean within-sample.
    total_var = between_var + mean_within_var
    sem = math.sqrt(total_var / k) if k else 0.0     # std error of the mean.
    low_confidence = (1.0 - mean(confidence_values)) if confidence_values else 0.0
    # One uncertainty scalar in [0,1]: the worst of dispersion and low confidence.
    uncertainty = min(1.0, max(math.sqrt(total_var), low_confidence))

    # 95% normal-approx interval on the mean (clamped to the unit interval).
    half = 1.96 * sem
    ci = [round(max(0.0, overall - half), 4), round(min(1.0, overall + half), 4)]

    near_threshold = (BLOCK_FLOOR <= overall <= ACCEPT_FLOOR)
    repeated = recommend_repeats(k, sem, uncertainty, hard_veto, near_threshold, target_sem)

    if hard_veto or overall < BLOCK_FLOOR:
        recommendation = "block"
    elif uncertainty >= 0.20 or overall < ACCEPT_FLOOR:
        recommendation = "escalate"
    else:
        recommendation = "accept"

    criteria_mean = {c: mean(v) for c, v in sorted(criteria_values.items())}
    criteria_unc = {c: pstdev(v) for c, v in sorted(criteria_values.items())}

    return {
        "id": candidate["id"],
        "overall_score": round(overall, 4),
        "uncertainty": round(uncertainty, 4),
        "std_error": round(sem, 4),
        "confidence_interval": ci,
        "hard_veto": hard_veto,
        "veto_reasons": veto_reasons,
        "criteria": {c: round(v, 4) for c, v in criteria_mean.items()},
        "criteria_uncertainty": {c: round(v, 4) for c, v in criteria_unc.items()},
        "samples": k,
        "repeated_evaluations_recommended": repeated,
        "recommendation": recommendation,
    }


# --- Bradley-Terry ranking (soft-win MLE) ----------------------------------

def _soft_win_matrix(candidates: List[Dict[str, Any]]) -> List[List[float]]:
    """Directed soft-win mass w[i][j] = expected wins of i over j from
    p = sigmoid(scale*(Ri-Rj)). Ring-pass slot symmetry is handled upstream by
    scoring both directions, so this all-pairs mass is the unbiased pairwise
    signal the O(Nk) tournament schedule approximates."""
    n = len(candidates)
    w = [[0.0] * n for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            ri = float(candidates[i]["overall_score"])
            rj = float(candidates[j]["overall_score"])
            w[i][j] = sigmoid(BT_SCALE * (ri - rj))
    return w


def bradley_terry(candidates: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Iterative Bradley-Terry MLE (MM / Zermelo) for latent strengths.

    Improvement over the paper's single-pass w_i/c_i average: fit strengths pi_i
    to the full soft-win matrix so the ranking is globally consistent (a win over
    a strong candidate counts more than one over a weak candidate). Returns rows
    with a normalised bt_score plus the paper's win-rate as a cross-check.
    """
    n = len(candidates)
    if n == 0:
        return []
    w = _soft_win_matrix(candidates)
    wins = [sum(w[i]) for i in range(n)]                       # Sum_j w[i][j]
    pi = [1.0] * n
    for _ in range(100):
        new = [0.0] * n
        for i in range(n):
            denom = 0.0
            for j in range(n):
                if i == j:
                    continue
                pair = w[i][j] + w[j][i]                       # total comparison mass
                denom += pair / (pi[i] + pi[j])
            new[i] = (wins[i] / denom) if denom > 0 else pi[i]
        s = sum(new) or 1.0
        new = [x / s * n for x in new]                          # keep scale anchored
        max_delta = max(abs(new[i] - pi[i]) for i in range(n))
        pi = new
        if max_delta < 1e-9:
            break

    total = sum(pi) or 1.0
    rows = []
    for i in range(n):
        win_rate = wins[i] / (n - 1) if n > 1 else 1.0         # paper's w_i/c_i
        rows.append(
            {
                "id": candidates[i]["id"],
                "bt_score": round(pi[i] / total, 4),           # MLE strength (normalised)
                "win_rate": round(win_rate, 4),
                "overall_score": candidates[i]["overall_score"],
                "uncertainty": candidates[i]["uncertainty"],
                "hard_veto": candidates[i]["hard_veto"],
            }
        )
    # A hard veto sinks a candidate below every non-vetoed one regardless of score.
    rows.sort(key=lambda r: (not r["hard_veto"], r["bt_score"]), reverse=True)
    return rows


# --- Probabilistic Pivot Tournament schedule (paper's PPT) -----------------

def pivot_tournament(
    candidates: List[Dict[str, Any]], pivots: int = 2, seed: int = 0
) -> Dict[str, Any]:
    """Emit the O(Nk) comparison SCHEDULE a harness executes: a ring pass over a
    random Hamiltonian cycle (cancels slot bias), then top-k pivot selection and
    pivot rounds. Improvement over a ring-only helper: the full two-phase plan
    plus the exact comparison budget N + k(N-k) + C(k,2)."""
    ids = [c["id"] for c in candidates]
    n = len(ids)
    if n < 2:
        return {"ring_pairs": [], "pivots": [], "pivot_pairs": [], "expected_comparisons": 0}

    k = max(1, min(pivots, n - 1))
    rng = random.Random(seed)
    order = ids[:]
    rng.shuffle(order)
    ring = [{"a": order[i], "b": order[(i + 1) % n]} for i in range(n)]

    # Pivots: top-k by aggregated overall (the ring supplies this in a live run;
    # here we reuse the already-aggregated scores). Vetoed candidates never pivot.
    ranked = sorted(
        candidates, key=lambda c: (not c["hard_veto"], c["overall_score"]), reverse=True
    )
    pivot_ids = [c["id"] for c in ranked[:k]]
    pivot_set = set(pivot_ids)

    pivot_pairs: List[Dict[str, str]] = []
    for other in ids:                                          # every non-pivot vs each pivot
        if other in pivot_set:
            continue
        for p in pivot_ids:
            pivot_pairs.append({"a": other, "b": p})
    for a in range(len(pivot_ids)):                            # pivots against each other
        for b in range(a + 1, len(pivot_ids)):
            pivot_pairs.append({"a": pivot_ids[a], "b": pivot_ids[b]})

    expected = n + k * (n - k) + (k * (k - 1)) // 2
    return {
        "ring_pairs": ring,
        "pivots": pivot_ids,
        "pivot_pairs": pivot_pairs,
        "expected_comparisons": expected,
        "complexity": f"O(Nk) — {expected} comparisons vs {n * (n - 1)} round-robin",
    }


# --- progress tracking (paper's progress) ----------------------------------

def _slope(ys: Sequence[float]) -> float:
    m = len(ys)
    if m < 2:
        return 0.0
    xs = list(range(m))
    mx, my = mean(xs), mean(ys)
    num = sum((xs[i] - mx) * (ys[i] - my) for i in range(m))
    den = sum((xs[i] - mx) ** 2 for i in range(m))
    return num / den if den else 0.0


def track_progress(blob: Any) -> Dict[str, Any]:
    """Per-step progress curve + trend classification + early-stop advice.

    Each step is a scalar in [0,1] or a {"logprobs": {...}} distribution over the
    A(0%)..T(100%) progress scale. Trend uses the slope of a trailing window;
    plateau is low trailing variance. Early-stop fires when enough steps have run,
    progress sits under `abandon_floor`, and the slope is non-positive.
    """
    if not isinstance(blob, dict):
        raise VerifierError("track input must be a JSON object")
    raw_steps = blob.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        raise VerifierError("steps must be a non-empty array")
    min_steps = int(blob.get("min_steps", 4))
    abandon_floor = clamp01(blob.get("abandon_floor", 0.10), "abandon_floor")
    window = max(2, int(blob.get("window", 3)))

    curve = [score_of(s, f"steps[{i}]", _LETTER_PROGRESS)[0] for i, s in enumerate(raw_steps)]

    trend: List[str] = []
    for i in range(len(curve)):
        w = curve[max(0, i - window + 1): i + 1]
        s = _slope(w)
        if i >= 1 and pstdev(w) < 0.02:
            trend.append("plateau")
        elif s > 0.02:
            trend.append("rising")
        elif s < -0.02:
            trend.append("regression")
        else:
            trend.append("flat")

    tail = curve[-window:]
    tail_slope = _slope(tail)
    last = curve[-1]
    if tail_slope > 0.02:
        overall_trend = "rising"
    elif tail_slope < -0.02:
        overall_trend = "regression"
    elif pstdev(tail) < 0.02:
        overall_trend = "plateau"
    else:
        overall_trend = "flat"

    early_stop = (len(curve) >= min_steps and last < abandon_floor and tail_slope <= 0.0)
    return {
        "schemaVersion": 1,
        "curve": [round(c, 4) for c in curve],
        "per_step_trend": trend,
        "final_progress": round(last, 4),
        "peak_progress": round(max(curve), 4),
        "tail_slope": round(tail_slope, 4),
        "overall_trend": overall_trend,
        "early_stop_recommended": early_stop,
    }


# --- top-level aggregate ---------------------------------------------------

def normalise_candidates(blob: Any) -> List[Dict[str, Any]]:
    if not isinstance(blob, dict):
        raise VerifierError("input must be a JSON object")
    raw = blob.get("candidates")
    if raw is None:
        raw = [{"id": blob.get("id", "candidate"), "samples": blob.get("samples")}]
    if not isinstance(raw, list) or not raw:
        raise VerifierError("candidates must be a non-empty array")
    out: List[Dict[str, Any]] = []
    for i, candidate in enumerate(raw):
        if not isinstance(candidate, dict):
            raise VerifierError(f"candidate {i} is not an object")
        samples = candidate.get("samples")
        if not isinstance(samples, list) or not samples:
            raise VerifierError(f"candidate {candidate.get('id', i)!r} has no samples")
        out.append({"id": str(candidate.get("id", f"candidate-{i+1}")), "samples": samples})
    return out


def resolve_weights(blob: Any) -> Dict[str, float]:
    weights = dict(DEFAULT_WEIGHTS)
    if isinstance(blob, dict) and isinstance(blob.get("weights"), dict):
        for key, value in blob["weights"].items():
            try:
                weights[str(key)] = float(value)
            except (TypeError, ValueError):
                raise VerifierError(f"weight for {key!r} must be numeric")
    return weights


def aggregate(blob: Any) -> Dict[str, Any]:
    weights = resolve_weights(blob)
    target_sem = DEFAULT_TARGET_SEM
    pivots, seed = 2, 0
    if isinstance(blob, dict):
        if "target_sem" in blob:
            target_sem = clamp01(blob["target_sem"], "target_sem")
        pivots = int(blob.get("pivots", 2))
        seed = int(blob.get("seed", 0))

    candidates = [aggregate_candidate(c, weights, target_sem) for c in normalise_candidates(blob)]
    ranking = bradley_terry(candidates) if len(candidates) > 1 else []
    tournament = pivot_tournament(candidates, pivots=pivots, seed=seed) if len(candidates) > 1 else {}
    return {
        "schemaVersion": 2,
        "candidates": candidates,
        "ranking": ranking,
        "pivot_tournament": tournament,
    }


def reward_cmd(blob: Any) -> Dict[str, float]:
    if isinstance(blob, dict) and "logprobs" in blob:
        return fine_grained_reward(blob["logprobs"])
    if isinstance(blob, dict) and "score" in blob:
        return {"reward": clamp01(blob["score"], "score"), "variance": 0.0, "valid_mass": 1.0}
    raise VerifierError("reward input needs a logprobs object or a score scalar")


# --- self-test -------------------------------------------------------------

def selftest() -> None:
    # (1) fine-grained reward: a peaked-at-A distribution rewards ~1.0, low var.
    fg = fine_grained_reward({"A": math.log(0.9), "B": math.log(0.1)})
    assert fg["reward"] > 0.9, fg
    assert fg["variance"] < 0.02, fg
    assert abs(fg["valid_mass"] - 1.0) < 1e-9, fg
    # A flat A-vs-T distribution sits mid-scale with HIGH variance.
    flat = fine_grained_reward({"A": math.log(0.5), "T": math.log(0.5)})
    assert abs(flat["reward"] - 0.5) < 1e-6, flat
    assert flat["variance"] > 0.2, flat
    # valid_mass drops when mass leaks onto a non-score token.
    leaky = fine_grained_reward({"A": math.log(0.5), "zzz": math.log(0.5)})
    assert leaky["valid_mass"] < 0.6, leaky

    # (2) aggregate: a safe multi-repeat candidate, a vetoed one, a logprob one.
    blob = {
        "candidates": [
            {"id": "safe-fix", "samples": [
                {"criteria": {"correctness": 0.9, "security": 1.0}, "confidence": 0.9},
                {"criteria": {"correctness": 0.8, "security": 1.0}, "confidence": 0.8},
            ]},
            {"id": "risky-fix", "samples": [
                {"criteria": {"correctness": 0.7, "security": 0.2}, "confidence": 0.6,
                 "hard_veto": True, "veto_reason": "security regression"},
            ]},
            {"id": "logprob-fix", "samples": [
                {"criteria": {"correctness": {"logprobs": {"A": math.log(0.8), "B": math.log(0.2)}},
                              "security": {"logprobs": {"A": math.log(0.95), "C": math.log(0.05)}}},
                 "confidence": 0.85},
            ]},
        ]
    }
    out = aggregate(blob)
    by_id = {c["id"]: c for c in out["candidates"]}
    assert by_id["safe-fix"]["recommendation"] in ("accept", "escalate"), by_id["safe-fix"]
    assert by_id["risky-fix"]["recommendation"] == "block", by_id["risky-fix"]
    assert by_id["risky-fix"]["hard_veto"] is True
    assert by_id["logprob-fix"]["overall_score"] > 0.7, by_id["logprob-fix"]
    # std_error present and the CI brackets the mean.
    s = by_id["safe-fix"]
    assert s["confidence_interval"][0] <= s["overall_score"] <= s["confidence_interval"][1]

    # (3) ranking: vetoed candidate never ranks first; a safe candidate leads.
    assert out["ranking"][0]["id"] in ("safe-fix", "logprob-fix"), out["ranking"]
    assert out["ranking"][-1]["id"] == "risky-fix", out["ranking"]

    # (4) pivot tournament budget is O(Nk), strictly below round-robin for N>k.
    tour = out["pivot_tournament"]
    assert tour["expected_comparisons"] == 3 + 2 * (3 - 2) + 1, tour  # N=3,k=2 -> 6
    assert len(tour["ring_pairs"]) == 3

    # (5) progress tracking: a rising curve is "rising", no early stop.
    rising = track_progress({"steps": [0.1, 0.3, 0.5, 0.7, 0.9]})
    assert rising["overall_trend"] == "rising", rising
    assert rising["early_stop_recommended"] is False
    # A stuck-low curve past min_steps recommends abandonment.
    stuck = track_progress({"steps": [0.05, 0.04, 0.03, 0.05, 0.02], "min_steps": 4})
    assert stuck["early_stop_recommended"] is True, stuck

    # (6) malformed inputs must FAIL (selftest: asserts-failure).
    for bad in (
        {"candidates": [{"id": "bad", "samples": []}]},          # empty samples
        {"candidates": [{"id": "x", "samples": [{"confidence": 0.5}]}]},  # no score
    ):
        try:
            aggregate(bad)
        except VerifierError:
            pass
        else:
            raise AssertionError(f"expected VerifierError for {bad!r}")
    try:
        fine_grained_reward({"zzz": math.log(1.0)})              # no valid tokens
    except VerifierError:
        pass
    else:
        raise AssertionError("expected VerifierError for token-less distribution")


# --- CLI -------------------------------------------------------------------

def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Aggregate LLM-as-a-Verifier samples (advisory).")
    parser.add_argument("command", nargs="?", help="aggregate | reward | track, or a samples path.")
    parser.add_argument("input", nargs="?", help="Input JSON path (when command is a mode).")
    parser.add_argument("--selftest", action="store_true", help="Run the deterministic self-test.")
    args = parser.parse_args(argv)

    modes = {"aggregate": aggregate, "reward": reward_cmd, "track": track_progress}
    try:
        if args.selftest:
            selftest()
            print("verifier-sidecar selftest: PASS")
            return 0

        if args.command in modes:
            path, fn = args.input, modes[args.command]
        elif args.command:                       # bare path -> aggregate (back-compat)
            path, fn = args.command, aggregate
        else:
            raise VerifierError("missing input JSON path")

        if not path:
            raise VerifierError("missing input JSON path")
        print(json.dumps(fn(load_json(path)), indent=2, sort_keys=True))
        return 0
    except (AssertionError, VerifierError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
