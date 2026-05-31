#!/usr/bin/env python3
# scripts/dev/agent-eval-score.py — score an agent eval run base-vs-head.
#
# Companion to scripts/dev/agent-eval-run.sh. One level up the stack from
# scripts/dev/perf-compare.py: it diffs agent DECISION QUALITY instead of frame
# latency, reusing the same load_json / evaluate / emit_markdown / 0-1-2
# exit-code skeleton. Plan: docs/plans/shipped/subagent-eval-harness.md.
#
# Consumes a BASE result JSON + a HEAD result JSON (per
# docs/agent-eval/result-schema.json). Each carries the agent's per-trial output
# + parsed findings for ONE case under ONE prompt-root. The two differ only in
# --prompt-root (base worktree vs head worktree) — that is the before/after seam.
#
# Two dimension kinds (declared in the case, per docs/agent-eval/case-schema.json):
#   * objective — a deterministic inline code check on trial.findings
#                 (cited_file_line / severity_enum / finding_count). No LLM.
#   * judge     — graded by an EXTERNAL judge command named by --judge-cmd /
#                 $SMATCHET_AGENT_EVAL_JUDGE_CMD. The scorer pipes
#                 {case, output, dimension, referenceOutcome} JSON to the
#                 command's stdin and parses {"score": 0..1} from its stdout.
#                 Keeping the judge external is what keeps THIS script pure
#                 stdlib + deterministic + unit-testable (plan § Approach step 2,
#                 RISK review finding 2).
#
# Scores are normalised to [0,1] (higher is better). A dimension REGRESSES when
# head drops below base by more than the policy's max_score_drop
# (docs/agent-eval/scoring-policy.json).
#
# Harness-agnostic — pure Python 3 stdlib, no third-party deps.
#
# Usage:
#   python scripts/dev/agent-eval-score.py <base-result.json> <head-result.json>
#                                          [--policy <path>] [--case <path>]
#                                          [--judge-cmd <cmd>] [--markdown-only]
#
# Exit codes:
#   0 — no dimension regressed beyond policy thresholds.
#   1 — at least one dimension regressed.
#   2 — usage / IO error / malformed input / judge failure.

from __future__ import annotations

import argparse
import io
import json
import os
import shlex
import subprocess
import sys
from typing import Any, Dict, List, Optional, Tuple

# Force UTF-8 on stdout — the markdown report uses the Greek delta (U+0394) and
# Windows' default cp1252 console encoding chokes on it (same fix as
# perf-compare.py).
if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )


# ----------------------------------------------------------------------------
# Defaults — mirror docs/agent-eval/scoring-policy.json if --policy not supplied.
# ----------------------------------------------------------------------------
DEFAULT_POLICY: Dict[str, Any] = {
    "max_score_drop": 0.05,
    "min_trials": 1,
}

DEFAULT_SEVERITIES = ["critical", "high", "medium", "low", "info"]


class ScoreError(ValueError):
    """Malformed input / judge failure — caught in main() -> exit 2."""


# ----------------------------------------------------------------------------
# IO
# ----------------------------------------------------------------------------
def load_json(path: str) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        sys.exit(2)
    except json.JSONDecodeError as e:
        print(f"ERROR: malformed JSON in {path}: {e}", file=sys.stderr)
        sys.exit(2)


def load_policy(path: Optional[str], agent: str) -> Dict[str, Any]:
    """Resolve thresholds. Precedence (later overlays earlier):
    default -> perAgent[agent]. perDimension is applied later, per-dimension,
    inside evaluate()."""
    policy = dict(DEFAULT_POLICY)
    raw: Dict[str, Any] = {}
    if path is not None and os.path.exists(path):
        loaded = load_json(path)
        if isinstance(loaded, dict):
            raw = loaded
    if isinstance(raw.get("default"), dict):
        policy.update(raw["default"])
    per_agent = raw.get("perAgent", {}) or {}
    if agent and isinstance(per_agent.get(agent), dict):
        policy.update(per_agent[agent])
    # Stash perDimension so evaluate() can overlay it per row.
    per_dim = raw.get("perDimension", {}) or {}
    policy["_perDimension"] = per_dim if isinstance(per_dim, dict) else {}
    return policy


def dimension_policy(policy: Dict[str, Any], dim_key: str) -> Dict[str, Any]:
    merged = {k: v for k, v in policy.items() if not k.startswith("_")}
    per_dim = policy.get("_perDimension", {})
    override = per_dim.get(dim_key)
    if isinstance(override, dict):
        merged.update(override)
    return merged


# ----------------------------------------------------------------------------
# Result / case extraction
# ----------------------------------------------------------------------------
def require_result(blob: Any, label: str) -> Dict[str, Any]:
    if not isinstance(blob, dict):
        raise ScoreError(f"{label}: result is not a JSON object")
    for key in ("caseId", "agent", "trials"):
        if key not in blob:
            raise ScoreError(f"{label}: missing required field '{key}'")
    if not isinstance(blob.get("trials"), list) or not blob["trials"]:
        raise ScoreError(f"{label}: 'trials' must be a non-empty array")
    return blob


def resolve_case(
    base: Dict[str, Any],
    head: Dict[str, Any],
    case_override: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    """Locate the golden case: explicit --case override wins, else the embedded
    snapshot in the head result, else the base result."""
    if case_override is not None:
        case = case_override
    elif isinstance(head.get("case"), dict):
        case = head["case"]
    elif isinstance(base.get("case"), dict):
        case = base["case"]
    else:
        raise ScoreError(
            "no case found: neither result embeds a 'case' object and --case not given"
        )
    if not isinstance(case.get("dimensions"), list) or not case["dimensions"]:
        raise ScoreError("case has no 'dimensions' array")
    return case


# ----------------------------------------------------------------------------
# Objective checks — deterministic, no LLM. Operate on one trial's findings.
# ----------------------------------------------------------------------------
def _findings(trial: Dict[str, Any]) -> List[Dict[str, Any]]:
    f = trial.get("findings")
    return [x for x in f if isinstance(x, dict)] if isinstance(f, list) else []


def check_cited_file_line(trial: Dict[str, Any], _case: Dict[str, Any]) -> float:
    fs = _findings(trial)
    if not fs:
        return 0.0
    cited = 0
    for f in fs:
        file_ok = isinstance(f.get("file"), str) and f["file"].strip() != ""
        line_ok = isinstance(f.get("line"), int) and f["line"] >= 1
        if file_ok and line_ok:
            cited += 1
    return cited / len(fs)


def check_severity_enum(trial: Dict[str, Any], case: Dict[str, Any]) -> float:
    fs = _findings(trial)
    if not fs:
        return 0.0
    ref = case.get("referenceOutcome", {}) or {}
    allowed = ref.get("allowedSeverities") or DEFAULT_SEVERITIES
    allowed_set = {str(s).lower() for s in allowed}
    ok = sum(1 for f in fs if str(f.get("severity", "")).lower() in allowed_set)
    return ok / len(fs)


def check_finding_count(trial: Dict[str, Any], case: Dict[str, Any]) -> float:
    fs = _findings(trial)
    ref = case.get("referenceOutcome", {}) or {}
    expected = ref.get("expectedFindingCount")
    if not isinstance(expected, int):
        raise ScoreError("finding_count check needs integer referenceOutcome.expectedFindingCount")
    n = len(fs)
    if expected == 0:
        return 1.0 if n == 0 else 0.0
    return max(0.0, 1.0 - abs(n - expected) / expected)


OBJECTIVE_CHECKS = {
    "cited_file_line": check_cited_file_line,
    "severity_enum": check_severity_enum,
    "finding_count": check_finding_count,
}


# ----------------------------------------------------------------------------
# Judge — external command. Pure-stdlib stays intact: we never call an LLM here.
# ----------------------------------------------------------------------------
def run_judge(
    judge_cmd: str,
    case: Dict[str, Any],
    trial: Dict[str, Any],
    dim_key: str,
) -> float:
    payload = json.dumps(
        {
            "dimension": dim_key,
            "output": trial.get("output", ""),
            "case": case,
            "referenceOutcome": case.get("referenceOutcome", {}),
        }
    )
    try:
        argv = shlex.split(judge_cmd, posix=(os.name != "nt"))
    except ValueError as exc:
        raise ScoreError(f"could not parse --judge-cmd {judge_cmd!r}: {exc}")
    if not argv:
        raise ScoreError("--judge-cmd is empty")
    try:
        proc = subprocess.run(
            argv,
            input=payload,
            capture_output=True,
            text=True,
        )
    except (OSError, ValueError) as exc:
        raise ScoreError(f"judge command failed to launch: {exc}")
    if proc.returncode != 0:
        raise ScoreError(
            f"judge command exited {proc.returncode} for dimension {dim_key!r}: "
            f"{proc.stderr.strip()[:200]}"
        )
    try:
        out = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise ScoreError(f"judge produced non-JSON output for {dim_key!r}: {exc}")
    if not isinstance(out, dict) or "score" not in out:
        raise ScoreError(f"judge output for {dim_key!r} missing 'score' field")
    try:
        score = float(out["score"])
    except (TypeError, ValueError):
        raise ScoreError(f"judge 'score' for {dim_key!r} is not numeric: {out['score']!r}")
    # Clamp into [0,1] so a misbehaving judge can't manufacture a fake delta.
    return max(0.0, min(1.0, score))


# ----------------------------------------------------------------------------
# Scoring
# ----------------------------------------------------------------------------
def score_result(
    result: Dict[str, Any],
    case: Dict[str, Any],
    dimensions: List[Dict[str, Any]],
    judge_cmd: Optional[str],
) -> Dict[str, float]:
    """Average each dimension's per-trial score across all trials in one result."""
    trials = result["trials"]
    scores: Dict[str, float] = {}
    for dim in dimensions:
        key = dim["key"]
        kind = dim.get("kind")
        per_trial: List[float] = []
        for trial in trials:
            if not isinstance(trial, dict):
                raise ScoreError("trial entry is not an object")
            if trial.get("error"):
                # A failed trial scores 0 on every dimension — a crash is the
                # worst possible quality outcome, not a skipped sample.
                per_trial.append(0.0)
                continue
            if kind == "objective":
                check_name = dim.get("check")
                fn = OBJECTIVE_CHECKS.get(check_name)
                if fn is None:
                    raise ScoreError(
                        f"dimension {key!r}: unknown objective check {check_name!r}"
                    )
                per_trial.append(fn(trial, case))
            elif kind == "judge":
                if not judge_cmd:
                    raise ScoreError(
                        f"dimension {key!r} is kind=judge but no --judge-cmd / "
                        "$SMATCHET_AGENT_EVAL_JUDGE_CMD was provided"
                    )
                per_trial.append(run_judge(judge_cmd, case, trial, key))
            else:
                raise ScoreError(f"dimension {key!r}: unknown kind {kind!r}")
        scores[key] = sum(per_trial) / len(per_trial) if per_trial else 0.0
    return scores


def evaluate(
    base_scores: Dict[str, float],
    head_scores: Dict[str, float],
    dimensions: List[Dict[str, Any]],
    n_trials: int,
    policy: Dict[str, Any],
) -> Tuple[List[Dict[str, Any]], List[str]]:
    rows: List[Dict[str, Any]] = []
    regressions: List[str] = []
    for dim in dimensions:
        key = dim["key"]
        kind = dim.get("kind", "?")
        b = base_scores.get(key, 0.0)
        h = head_scores.get(key, 0.0)
        delta = h - b
        dp = dimension_policy(policy, key)
        max_drop = float(dp.get("max_score_drop", 0.05))
        min_trials = int(dp.get("min_trials", 1))
        is_regression = False
        # Too few trials to trust the delta — show the row, don't gate on it
        # (mirrors perf-compare's min_baseline_calls noise floor).
        if n_trials >= min_trials and delta < -max_drop:
            is_regression = True
            regressions.append(
                f"{key} ({kind}): regressed {delta:+.3f} "
                f"(base {b:.3f} -> head {h:.3f}, policy max drop {max_drop:.3f})"
            )
        rows.append(
            {
                "key": key,
                "kind": kind,
                "base": b,
                "head": h,
                "delta": delta,
                "regression": is_regression,
            }
        )
    return rows, regressions


# ----------------------------------------------------------------------------
# Markdown
# ----------------------------------------------------------------------------
def emit_markdown(
    case: Dict[str, Any],
    base: Dict[str, Any],
    head: Dict[str, Any],
    rows: List[Dict[str, Any]],
    regressions: List[str],
    n_trials: int,
) -> str:
    lines: List[str] = []
    case_id = case.get("caseId", base.get("caseId", "<unknown>"))
    agent = case.get("agent", base.get("agent", "<unknown>"))
    lines.append(f"### Agent eval delta — `{agent}` · case `{case_id}`")
    lines.append("")
    lines.append(f"- base prompt-root: `{base.get('promptRoot', '?')}` (harness `{base.get('harness', '?')}`)")
    lines.append(f"- head prompt-root: `{head.get('promptRoot', '?')}` (harness `{head.get('harness', '?')}`)")
    lines.append(f"- trials: {n_trials} · scores normalised to [0,1] (higher is better)")
    lines.append("")
    lines.append("| dimension | kind | base | head | Δ | verdict |")
    lines.append("|---|---|---:|---:|---:|---|")
    for r in rows:
        verdict = "**REGRESSION**" if r["regression"] else "ok"
        sign = "+" if r["delta"] >= 0 else ""
        lines.append(
            f"| `{r['key']}` | {r['kind']} | {r['base']:.3f} | {r['head']:.3f} | "
            f"{sign}{r['delta']:.3f} | {verdict} |"
        )
    lines.append("")
    if regressions:
        lines.append(f"**Regressions: {len(regressions)}** (advisory — WARN, not BLOCK, until calibrated)")
        lines.append("")
        for r in regressions:
            lines.append(f"- {r}")
    else:
        lines.append("**No regressions.**")
    lines.append("")
    return "\n".join(lines)


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------
def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Score an agent eval run base-vs-head across quality dimensions."
    )
    parser.add_argument("base", help="Path to the BASE result JSON (per result-schema.json).")
    parser.add_argument("head", help="Path to the HEAD result JSON.")
    parser.add_argument(
        "--policy",
        default=os.path.join("docs", "agent-eval", "scoring-policy.json"),
        help="Scoring policy JSON. Default: docs/agent-eval/scoring-policy.json.",
    )
    parser.add_argument(
        "--case",
        default=None,
        help="Override case JSON. Default: read the embedded 'case' snapshot in the results.",
    )
    parser.add_argument(
        "--judge-cmd",
        default=os.environ.get("SMATCHET_AGENT_EVAL_JUDGE_CMD"),
        help="External judge command for kind=judge dimensions. "
        "Reads {case,output,dimension} JSON on stdin, prints {\"score\":0..1} on stdout. "
        "Falls back to $SMATCHET_AGENT_EVAL_JUDGE_CMD.",
    )
    parser.add_argument(
        "--markdown-only",
        action="store_true",
        help="Print the markdown report; always exit 0 (advisory mode).",
    )
    args = parser.parse_args(argv)

    base_raw = load_json(args.base)
    head_raw = load_json(args.head)
    case_override = load_json(args.case) if args.case else None

    try:
        base = require_result(base_raw, args.base)
        head = require_result(head_raw, args.head)
        if base["caseId"] != head["caseId"]:
            raise ScoreError(
                f"caseId mismatch: base {base['caseId']!r} != head {head['caseId']!r}"
            )
        case = resolve_case(base, head, case_override)
        dimensions = case["dimensions"]
        for dim in dimensions:
            if not isinstance(dim, dict) or "key" not in dim:
                raise ScoreError("each dimension needs a 'key'")

        policy = load_policy(args.policy, case.get("agent", base.get("agent", "")))
        base_scores = score_result(base, case, dimensions, args.judge_cmd)
        head_scores = score_result(head, case, dimensions, args.judge_cmd)
        n_trials = min(len(base["trials"]), len(head["trials"]))
        rows, regressions = evaluate(base_scores, head_scores, dimensions, n_trials, policy)
    except ScoreError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(emit_markdown(case, base, head, rows, regressions, n_trials))

    if args.markdown_only:
        return 0
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
