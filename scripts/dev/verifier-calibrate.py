#!/usr/bin/env python3
# scripts/dev/verifier-calibrate.py — is the verifier trustworthy enough to
# promote from advisory to blocking? Pure Python 3 stdlib, deterministic, offline.
# Docs: docs/agent-rules/verifier-sidecar.md § Operating rules (rule 5).
#
# The verifier sidecar + producer emit continuous [0,1] scores; this closes the
# loop the same way agent-eval-calibrate.py does for the eval judge, but with the
# metrics that fit a PROBABILISTIC forecaster rather than a divergence gate:
#
#   * Brier score — mean((score - outcome)^2); the standard proper score for a
#     probability forecast (0 = perfect, 0.25 = always-guess-0.5, 1 = worst).
#   * ECE (Expected Calibration Error) — bin the scores; per bin compare the mean
#     predicted score against the empirical outcome rate; the count-weighted gap.
#     Answers "when it says 0.8, does it happen 80% of the time?".
#   * AUC (rank / Mann-Whitney) — does it rank real-good above real-bad at all?
#   * hard-veto precision / recall — the governance channel: do vetoes land on
#     genuinely bad candidates, and do they catch the bad ones?
#
# It stays ADVISORY: by default it reports and exits 0. Promotion to blocking is
# a human decision informed by this report; pass --gate to make "not yet
# eligible" a hard exit 1 (e.g. a CI job that guards a future blocking switch).
#
# Input — a calibration set pairing each verifier score with a ground-truth
# outcome (1 = the candidate was actually good/correct/safe, 0 = not; a
# continuous [0,1] label is accepted too):
#   {
#     "threshold": 0.6,          # optional; decision threshold for accuracy
#     "policy": {                # optional; overrides the promotion thresholds
#       "min_samples": 20, "max_brier": 0.15, "max_ece": 0.10, "min_auc": 0.75
#     },
#     "cases": [
#       {"id": "fix-a", "score": 0.82, "outcome": 1, "hard_veto": false},
#       {"id": "fix-b", "score": 0.30, "outcome": 0, "hard_veto": true}
#     ]
#   }
#
# Or join a sidecar aggregate output with a labels file by candidate id:
#   python scripts/dev/verifier-calibrate.py --scores agg.json --labels labels.json
#   # labels.json: {"fix-a": 1, "fix-b": 0}   (or {"fix-a": {"outcome": 1}})
#
# Usage:
#   python scripts/dev/verifier-calibrate.py calibration.json
#   python scripts/dev/verifier-calibrate.py --scores agg.json --labels labels.json
#   python scripts/dev/verifier-calibrate.py calibration.json --gate
#   python scripts/dev/verifier-calibrate.py --selftest
#
# selftest: asserts-failure — --selftest exercises a not-eligible (--gate BLOCK) case.
#
# Exit codes:
#   0 — report emitted (advisory), or --gate and the verifier is eligible.
#   1 — --gate and the verifier is NOT yet eligible for blocking promotion.
#   2 — usage / IO error / malformed input.

from __future__ import annotations

import argparse
import io
import json
import math
import sys
from typing import Any, Dict, List, Optional

if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )

DEFAULT_POLICY: Dict[str, float] = {
    "min_samples": 20,     # too few labels ⇒ can't trust the estimate yet.
    "max_brier": 0.15,     # forecast sharpness/accuracy ceiling.
    "max_ece": 0.10,       # reliability: predicted prob must track reality.
    "min_auc": 0.75,       # must at least rank good above bad.
}
DEFAULT_THRESHOLD = 0.6
DEFAULT_BINS = 10


class CalibrationError(ValueError):
    """Malformed calibration input."""


def _read(path: str) -> str:
    if path == "-":
        return sys.stdin.buffer.read().decode("utf-8")
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def load_json(path: str) -> Any:
    try:
        return json.loads(_read(path))
    except FileNotFoundError as exc:
        raise CalibrationError(f"file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise CalibrationError(f"malformed JSON in {path}: {exc}") from exc


def _unit(value: Any, field: str) -> float:
    try:
        v = float(value)
    except (TypeError, ValueError) as exc:
        raise CalibrationError(f"{field} must be numeric") from exc
    if not math.isfinite(v):
        raise CalibrationError(f"{field} must be finite")
    return max(0.0, min(1.0, v))


# --- assembling the (score, outcome, veto) rows ----------------------------

def _label_outcome(raw: Any, cid: str) -> float:
    if isinstance(raw, dict):
        if "outcome" not in raw:
            raise CalibrationError(f"label for {cid!r} has no 'outcome'")
        return _unit(raw["outcome"], f"label[{cid}].outcome")
    return _unit(raw, f"label[{cid}]")


def rows_from_cases(blob: Any) -> List[Dict[str, Any]]:
    if not isinstance(blob, dict):
        raise CalibrationError("input must be a JSON object")
    cases = blob.get("cases")
    if not isinstance(cases, list) or not cases:
        raise CalibrationError("cases must be a non-empty array")
    rows: List[Dict[str, Any]] = []
    seen = set()
    for i, c in enumerate(cases):
        if not isinstance(c, dict):
            raise CalibrationError(f"case {i} is not an object")
        cid = str(c.get("id", f"case-{i+1}"))
        if cid in seen:
            raise CalibrationError(f"duplicate case id {cid!r}")
        seen.add(cid)
        if "score" not in c or "outcome" not in c:
            raise CalibrationError(f"case {cid!r} needs both 'score' and 'outcome'")
        veto = c.get("hard_veto", False)
        if not isinstance(veto, bool):
            raise CalibrationError(f"case {cid!r} hard_veto must be boolean")
        rows.append({
            "id": cid,
            "score": _unit(c["score"], f"case[{cid}].score"),
            "outcome": _unit(c["outcome"], f"case[{cid}].outcome"),
            "hard_veto": veto,
        })
    return rows


def rows_from_join(agg: Any, labels: Any) -> List[Dict[str, Any]]:
    """Join a sidecar aggregate output (candidates[].overall_score/hard_veto)
    with a {id: label} map by candidate id."""
    if not isinstance(agg, dict) or not isinstance(agg.get("candidates"), list):
        raise CalibrationError("--scores must be a sidecar aggregate object")
    if not isinstance(labels, dict) or not labels:
        raise CalibrationError("--labels must be a non-empty {id: outcome} object")
    rows: List[Dict[str, Any]] = []
    for cand in agg["candidates"]:
        cid = str(cand.get("id"))
        if cid not in labels:
            continue  # unlabelled candidates are skipped, not an error.
        rows.append({
            "id": cid,
            "score": _unit(cand.get("overall_score"), f"{cid}.overall_score"),
            "outcome": _label_outcome(labels[cid], cid),
            "hard_veto": bool(cand.get("hard_veto", False)),
        })
    if not rows:
        raise CalibrationError("no candidate id in --scores matched a --labels entry")
    return rows


# --- metrics ---------------------------------------------------------------

def brier(rows: List[Dict[str, Any]]) -> float:
    return sum((r["score"] - r["outcome"]) ** 2 for r in rows) / len(rows)


def reliability(rows: List[Dict[str, Any]], bins: int) -> List[Dict[str, Any]]:
    buckets: List[List[Dict[str, Any]]] = [[] for _ in range(bins)]
    for r in rows:
        idx = min(bins - 1, int(r["score"] * bins))
        buckets[idx].append(r)
    table = []
    for b, items in enumerate(buckets):
        if not items:
            continue
        table.append({
            "bin": f"[{b/bins:.1f},{(b+1)/bins:.1f})",
            "count": len(items),
            "mean_score": round(sum(i["score"] for i in items) / len(items), 4),
            "empirical_rate": round(sum(i["outcome"] for i in items) / len(items), 4),
        })
    return table


def ece(rows: List[Dict[str, Any]], bins: int) -> float:
    n = len(rows)
    total = 0.0
    for row in reliability(rows, bins):
        total += (row["count"] / n) * abs(row["mean_score"] - row["empirical_rate"])
    return total


def auc(rows: List[Dict[str, Any]]) -> Optional[float]:
    """Rank-based AUC (Mann-Whitney U). Needs both a positive and a negative
    class (outcome >= 0.5 vs < 0.5); None when only one class is present."""
    pos = [r["score"] for r in rows if r["outcome"] >= 0.5]
    neg = [r["score"] for r in rows if r["outcome"] < 0.5]
    if not pos or not neg:
        return None
    # Sum over pairs of 1[pos>neg] + 0.5*1[pos==neg], normalised.
    ranked = sorted(((r["score"], r["outcome"] >= 0.5) for r in rows), key=lambda t: t[0])
    # Assign average ranks (1-based) handling ties.
    ranks: Dict[int, float] = {}
    i = 0
    while i < len(ranked):
        j = i
        while j + 1 < len(ranked) and ranked[j + 1][0] == ranked[i][0]:
            j += 1
        avg = (i + j) / 2.0 + 1.0  # 1-based average rank
        for k in range(i, j + 1):
            ranks[k] = avg
        i = j + 1
    rank_sum_pos = sum(ranks[k] for k, (_, is_pos) in enumerate(ranked) if is_pos)
    n_pos, n_neg = len(pos), len(neg)
    u = rank_sum_pos - n_pos * (n_pos + 1) / 2.0
    return u / (n_pos * n_neg)


def veto_stats(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    vetoed = [r for r in rows if r["hard_veto"]]
    bad = [r for r in rows if r["outcome"] < 0.5]
    tp = sum(1 for r in vetoed if r["outcome"] < 0.5)
    precision = (tp / len(vetoed)) if vetoed else None       # of vetoes, share truly bad
    recall = (tp / len(bad)) if bad else None                # of bad, share vetoed
    return {
        "vetoed": len(vetoed),
        "precision": round(precision, 4) if precision is not None else None,
        "recall": round(recall, 4) if recall is not None else None,
    }


def accuracy(rows: List[Dict[str, Any]], threshold: float) -> float:
    correct = sum(1 for r in rows if (r["score"] >= threshold) == (r["outcome"] >= 0.5))
    return correct / len(rows)


def calibrate(rows: List[Dict[str, Any]], policy: Dict[str, float],
              threshold: float, bins: int) -> Dict[str, Any]:
    n = len(rows)
    b = brier(rows)
    e = ece(rows, bins)
    a = auc(rows)
    reasons: List[str] = []
    if n < policy["min_samples"]:
        reasons.append(f"only {n} samples (need >= {int(policy['min_samples'])})")
    if b > policy["max_brier"]:
        reasons.append(f"Brier {b:.3f} > {policy['max_brier']}")
    if e > policy["max_ece"]:
        reasons.append(f"ECE {e:.3f} > {policy['max_ece']}")
    if a is None:
        reasons.append("AUC undefined (need both good and bad outcomes)")
    elif a < policy["min_auc"]:
        reasons.append(f"AUC {a:.3f} < {policy['min_auc']}")
    eligible = not reasons
    return {
        "schemaVersion": 1,
        "samples": n,
        "threshold": threshold,
        "accuracy": round(accuracy(rows, threshold), 4),
        "brier": round(b, 4),
        "ece": round(e, 4),
        "auc": round(a, 4) if a is not None else None,
        "hard_veto": veto_stats(rows),
        "reliability": reliability(rows, bins),
        "policy": policy,
        "eligible_for_blocking": eligible,
        "recommendation": "eligible-for-blocking" if eligible else "stay-advisory",
        "blockers": reasons,
    }


# --- self-test -------------------------------------------------------------

def selftest() -> None:
    # A well-calibrated, discriminating verifier: scores track outcomes, vetoes
    # land on bad cases. Should be eligible for promotion.
    good = {"cases": []}
    for i in range(24):
        y = 1 if i % 2 == 0 else 0
        # score hugs the outcome with a little noise → low Brier, low ECE, high AUC.
        s = 0.95 - 0.02 * (i % 3) if y == 1 else 0.05 + 0.02 * (i % 3)
        good["cases"].append({"id": f"c{i}", "score": s, "outcome": y,
                              "hard_veto": (y == 0 and i % 4 == 1)})
    rep = calibrate(rows_from_cases(good), dict(DEFAULT_POLICY), DEFAULT_THRESHOLD, DEFAULT_BINS)
    assert rep["eligible_for_blocking"] is True, rep["blockers"]
    assert rep["auc"] == 1.0, rep
    assert rep["brier"] < 0.15 and rep["ece"] < 0.10, rep
    assert rep["accuracy"] == 1.0, rep
    assert rep["hard_veto"]["precision"] == 1.0, rep["hard_veto"]

    # A random/miscalibrated verifier: scores unrelated to outcomes → not eligible.
    noisy = {"cases": [{"id": f"n{i}", "score": 0.5, "outcome": i % 2} for i in range(24)]}
    rep2 = calibrate(rows_from_cases(noisy), dict(DEFAULT_POLICY), DEFAULT_THRESHOLD, DEFAULT_BINS)
    assert rep2["eligible_for_blocking"] is False, rep2
    assert any("AUC" in r or "Brier" in r for r in rep2["blockers"]), rep2["blockers"]

    # Too few samples blocks promotion even when perfect.
    tiny = {"cases": [{"id": "a", "score": 0.9, "outcome": 1},
                      {"id": "b", "score": 0.1, "outcome": 0}]}
    rep3 = calibrate(rows_from_cases(tiny), dict(DEFAULT_POLICY), DEFAULT_THRESHOLD, DEFAULT_BINS)
    assert rep3["eligible_for_blocking"] is False, rep3
    assert any("samples" in r for r in rep3["blockers"]), rep3["blockers"]

    # AUC is None with a single class.
    assert auc(rows_from_cases({"cases": [{"id": "x", "score": 0.9, "outcome": 1}]})) is None

    # Join path.
    agg = {"candidates": [{"id": "fix-a", "overall_score": 0.9, "hard_veto": False},
                          {"id": "fix-b", "overall_score": 0.2, "hard_veto": True}]}
    joined = rows_from_join(agg, {"fix-a": 1, "fix-b": {"outcome": 0}})
    assert {r["id"] for r in joined} == {"fix-a", "fix-b"}, joined

    # Malformed inputs FAIL (selftest: asserts-failure).
    for bad in (
        {"cases": []},                                             # empty
        {"cases": [{"id": "a", "score": 0.5}]},                    # no outcome
        {"cases": [{"id": "a", "score": 0.5, "outcome": 1, "hard_veto": "no"}]},  # non-bool
        {"cases": [{"id": "d", "score": 0.5, "outcome": 1},
                   {"id": "d", "score": 0.6, "outcome": 0}]},      # dup id
    ):
        try:
            rows_from_cases(bad)
        except CalibrationError:
            pass
        else:
            raise AssertionError(f"expected CalibrationError for {bad!r}")
    try:
        rows_from_join({"candidates": [{"id": "z", "overall_score": 0.5}]}, {"other": 1})
    except CalibrationError:
        pass
    else:
        raise AssertionError("join with no id overlap must fail")


# --- CLI -------------------------------------------------------------------

def resolve_policy(blob: Any) -> Dict[str, float]:
    policy = dict(DEFAULT_POLICY)
    if isinstance(blob, dict) and isinstance(blob.get("policy"), dict):
        for k, v in blob["policy"].items():
            if k not in DEFAULT_POLICY:
                raise CalibrationError(f"unknown policy key {k!r}")
            try:
                policy[k] = float(v)
            except (TypeError, ValueError) as exc:
                raise CalibrationError(f"policy.{k} must be numeric") from exc
    return policy


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Calibrate the verifier: advisory vs blocking readiness.")
    parser.add_argument("input", nargs="?", help="Calibration JSON path, or '-' for stdin.")
    parser.add_argument("--scores", help="Sidecar aggregate output to join with --labels.")
    parser.add_argument("--labels", help="{id: outcome} labels to join with --scores.")
    parser.add_argument("--bins", type=int, default=DEFAULT_BINS, help="Reliability bins (default 10).")
    parser.add_argument("--gate", action="store_true", help="Exit 1 when not eligible for blocking.")
    parser.add_argument("--selftest", action="store_true", help="Run the deterministic self-test.")
    args = parser.parse_args(argv)

    try:
        if args.selftest:
            selftest()
            print("verifier-calibrate selftest: PASS")
            return 0

        threshold = DEFAULT_THRESHOLD
        if args.scores or args.labels:
            if not (args.scores and args.labels):
                raise CalibrationError("--scores and --labels must be used together")
            rows = rows_from_join(load_json(args.scores), load_json(args.labels))
            policy = dict(DEFAULT_POLICY)
        else:
            if not args.input:
                raise CalibrationError("missing calibration JSON path (or '-' for stdin)")
            blob = load_json(args.input)
            rows = rows_from_cases(blob)
            policy = resolve_policy(blob)
            if isinstance(blob, dict) and "threshold" in blob:
                threshold = _unit(blob["threshold"], "threshold")

        if args.bins < 1:
            raise CalibrationError("--bins must be >= 1")
        report = calibrate(rows, policy, threshold, args.bins)
        print(json.dumps(report, indent=2, sort_keys=True))
        if args.gate and not report["eligible_for_blocking"]:
            print("verifier-calibrate: NOT eligible for blocking promotion — "
                  + "; ".join(report["blockers"]), file=sys.stderr)
            return 1
        return 0
    except (AssertionError, CalibrationError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
