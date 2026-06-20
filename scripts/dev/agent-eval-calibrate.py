#!/usr/bin/env python3
# scripts/dev/agent-eval-calibrate.py — judge-vs-human calibration gate.
#
# Companion to scripts/dev/agent-eval-score.py. The scorer answers "did this
# prompt edit regress?"; THIS answers the prior question "can we trust the judge
# at all?". It is the deferred WARN->BLOCK enabler named in the plan
# (docs/plans/shipped/subagent-eval-harness.md § Verification "Manual residue":
# "judge-vs-human calibration") and the subagent-eval-calibration backlog entry.
#
# WHAT IT DOES
#   Reads a human-labelled calibration set (docs/agent-eval/calibration/<agent>/
#   labels.json). Each entry pins a RECORDED result JSON (the judge INPUT, frozen
#   offline so CI needs no live model call) plus the score a human gave each
#   JUDGE-kind dimension on that exact trial output. For each (entry, dimension)
#   it runs the EXTERNAL judge command (same seam as agent-eval-score.py:
#   --judge-cmd / $SMATCHET_AGENT_EVAL_JUDGE_CMD; pipes {case,output,dimension,
#   referenceOutcome} in, parses {"score":0..1} out), then measures the
#   divergence |judge - human|.
#
# THE GATE (both must hold; BLOCKS, unlike the advisory scorer)
#   (1) mean |judge - human| across all pairs <= policy.max_mean_divergence
#   (2) no single pair diverges by more than policy.max_single_divergence
#   A judge that disagrees with humans beyond tolerance is not trustworthy enough
#   to gate prompt-quality regressions, so calibration failing is a HARD error.
#
# Pure Python 3 stdlib + deterministic + offline-testable: the judge is external
# (a recorded/fake judge in CI, a real model off-CI), exactly like the scorer.
# The objective dimensions are excluded — they are deterministic code checks with
# no judge to calibrate.
#
# Usage:
#   python scripts/dev/agent-eval-calibrate.py <labels.json>
#       [--policy <path>] [--judge-cmd <cmd>] [--markdown-only]
#   python scripts/dev/agent-eval-calibrate.py --selftest
#
# Exit codes:
#   0 — judge is calibrated within policy (or --markdown-only / --selftest pass).
#   1 — judge diverges from humans beyond policy (BLOCK).
#   2 — usage / IO error / malformed input / judge failure.
#
# selftest: asserts-failure — --selftest exercises an over-threshold (BLOCK) case.

from __future__ import annotations

import argparse
import io
import json
import os
import shlex
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

# Force UTF-8 on stdout — the markdown report uses the Greek delta (U+0394) and
# Windows' default cp1252 console encoding chokes on it (same fix as the scorer).
if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )


# Defaults — mirror docs/agent-eval/calibration-policy.json if --policy absent.
DEFAULT_POLICY: Dict[str, Any] = {
    "max_mean_divergence": 0.15,
    "max_single_divergence": 0.25,
}


class CalibError(ValueError):
    """Malformed input / judge failure — caught in main() -> exit 2."""


# ----------------------------------------------------------------------------
# IO + policy (same shape as agent-eval-score.py)
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
    default -> perAgent[agent]. perDimension is overlaid per pair in evaluate()."""
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
    per_dim = raw.get("perDimension", {}) or {}
    policy["_perDimension"] = per_dim if isinstance(per_dim, dict) else {}
    return policy


def dimension_policy(policy: Dict[str, Any], dim_key: str) -> Dict[str, Any]:
    merged = {k: v for k, v in policy.items() if not k.startswith("_")}
    override = policy.get("_perDimension", {}).get(dim_key)
    if isinstance(override, dict):
        merged.update(override)
    return merged


# ----------------------------------------------------------------------------
# Judge — external command (identical contract to agent-eval-score.run_judge).
# ----------------------------------------------------------------------------
def run_judge(judge_cmd: str, case: Dict[str, Any], output: str, dim_key: str) -> float:
    payload = json.dumps(
        {
            "dimension": dim_key,
            "output": output,
            "case": case,
            "referenceOutcome": case.get("referenceOutcome", {}),
        }
    )
    try:
        argv = shlex.split(judge_cmd, posix=(os.name != "nt"))
    except ValueError as exc:
        raise CalibError(f"could not parse --judge-cmd {judge_cmd!r}: {exc}")
    if not argv:
        raise CalibError("--judge-cmd is empty")
    try:
        proc = subprocess.run(argv, input=payload, capture_output=True, text=True)
    except (OSError, ValueError) as exc:
        raise CalibError(f"judge command failed to launch: {exc}")
    if proc.returncode != 0:
        raise CalibError(
            f"judge command exited {proc.returncode} for dimension {dim_key!r}: "
            f"{proc.stderr.strip()[:200]}"
        )
    try:
        out = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise CalibError(f"judge produced non-JSON output for {dim_key!r}: {exc}")
    if not isinstance(out, dict) or "score" not in out:
        raise CalibError(f"judge output for {dim_key!r} missing 'score' field")
    try:
        score = float(out["score"])
    except (TypeError, ValueError):
        raise CalibError(f"judge 'score' for {dim_key!r} is not numeric: {out['score']!r}")
    return max(0.0, min(1.0, score))


# ----------------------------------------------------------------------------
# Labels resolution
# ----------------------------------------------------------------------------
def _trial_output(result: Dict[str, Any], trial_index: int, label: str) -> str:
    trials = result.get("trials")
    if not isinstance(trials, list) or not trials:
        raise CalibError(f"{label}: recorded result has no 'trials' array")
    chosen = None
    for t in trials:
        if isinstance(t, dict) and t.get("index") == trial_index:
            chosen = t
            break
    if chosen is None:
        # Fall back to positional if no matching index field.
        if 0 <= trial_index < len(trials) and isinstance(trials[trial_index], dict):
            chosen = trials[trial_index]
    if chosen is None:
        raise CalibError(f"{label}: no trial with index {trial_index}")
    out = chosen.get("output")
    if not isinstance(out, str):
        raise CalibError(f"{label}: trial {trial_index} has no string 'output'")
    return out


def _result_case(result: Dict[str, Any], label: str) -> Dict[str, Any]:
    case = result.get("case")
    if isinstance(case, dict):
        return case
    # The recorded result may omit the embedded case; synthesise a minimal one so
    # the judge still receives a referenceOutcome-shaped payload.
    return {
        "caseId": result.get("caseId", "<unknown>"),
        "agent": result.get("agent", "<unknown>"),
        "referenceOutcome": {},
    }


def collect_pairs(
    labels: Dict[str, Any],
    labels_dir: str,
    judge_cmd: str,
) -> List[Dict[str, Any]]:
    """For each (entry, human-scored dimension) run the judge and record the
    judge score, human score, and |divergence|."""
    entries = labels.get("entries")
    if not isinstance(entries, list) or not entries:
        raise CalibError("labels file has no non-empty 'entries' array")
    pairs: List[Dict[str, Any]] = []
    for ei, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise CalibError(f"entry {ei} is not an object")
        label = entry.get("label", f"entry-{ei}")
        result_ref = entry.get("result")
        if not isinstance(result_ref, str) or not result_ref:
            raise CalibError(f"{label}: missing 'result' path")
        # Resolve the recorded-result path relative to the labels file's dir,
        # then relative to the repo cwd — whichever exists.
        cand = result_ref
        if not os.path.isabs(cand) and not os.path.exists(cand):
            joined = os.path.join(labels_dir, os.path.basename(result_ref))
            cand = joined if os.path.exists(joined) else result_ref
        result = load_json(cand)
        if not isinstance(result, dict):
            raise CalibError(f"{label}: recorded result is not a JSON object")
        trial_index = entry.get("trialIndex", 0)
        if not isinstance(trial_index, int):
            raise CalibError(f"{label}: 'trialIndex' must be an integer")
        human = entry.get("human")
        if not isinstance(human, dict) or not human:
            raise CalibError(f"{label}: missing non-empty 'human' score map")
        output = _trial_output(result, trial_index, label)
        case = _result_case(result, label)
        for dim_key, human_raw in human.items():
            try:
                human_score = float(human_raw)
            except (TypeError, ValueError):
                raise CalibError(f"{label}: human score for {dim_key!r} is not numeric")
            if not (0.0 <= human_score <= 1.0):
                raise CalibError(f"{label}: human score for {dim_key!r} not in [0,1]")
            judge_score = run_judge(judge_cmd, case, output, dim_key)
            pairs.append(
                {
                    "label": label,
                    "dimension": dim_key,
                    "human": human_score,
                    "judge": judge_score,
                    "divergence": abs(judge_score - human_score),
                }
            )
    return pairs


# ----------------------------------------------------------------------------
# Evaluate
# ----------------------------------------------------------------------------
def evaluate(
    pairs: List[Dict[str, Any]], policy: Dict[str, Any]
) -> Tuple[float, List[str]]:
    """Return (mean_divergence, violations). Empty violations => calibrated."""
    if not pairs:
        raise CalibError("no (entry,dimension) pairs to calibrate — empty corpus")
    violations: List[str] = []
    total = 0.0
    # Per-pair single-divergence gate (perDimension override applies here).
    for p in pairs:
        total += p["divergence"]
        dp = dimension_policy(policy, p["dimension"])
        max_single = float(dp.get("max_single_divergence", 0.25))
        if p["divergence"] > max_single:
            violations.append(
                f"{p['label']} / {p['dimension']}: divergence {p['divergence']:.3f} "
                f"exceeds max_single {max_single:.3f} "
                f"(judge {p['judge']:.3f} vs human {p['human']:.3f})"
            )
    mean_div = total / len(pairs)
    max_mean = float(
        {k: v for k, v in policy.items() if not k.startswith("_")}.get(
            "max_mean_divergence", 0.15
        )
    )
    if mean_div > max_mean:
        violations.append(
            f"mean divergence {mean_div:.3f} across {len(pairs)} pairs "
            f"exceeds max_mean {max_mean:.3f}"
        )
    return mean_div, violations


def emit_markdown(
    labels: Dict[str, Any],
    pairs: List[Dict[str, Any]],
    mean_div: float,
    violations: List[str],
) -> str:
    agent = labels.get("agent", "<unknown>")
    lines: List[str] = []
    lines.append(f"### Judge calibration — `{agent}` (judge vs human)")
    lines.append("")
    lines.append(f"- pairs: {len(pairs)} · mean |Δ|: {mean_div:.3f} · lower is better")
    lines.append("")
    lines.append("| entry | dimension | human | judge | |Δ| | verdict |")
    lines.append("|---|---|---:|---:|---:|---|")
    flagged = {(v.split(" / ")[0], v.split(" / ")[1].split(":")[0]) for v in violations if " / " in v}
    for p in pairs:
        bad = (p["label"], p["dimension"]) in flagged
        verdict = "**OVER**" if bad else "ok"
        lines.append(
            f"| {p['label']} | `{p['dimension']}` | {p['human']:.3f} | "
            f"{p['judge']:.3f} | {p['divergence']:.3f} | {verdict} |"
        )
    lines.append("")
    if violations:
        lines.append(f"**BLOCK: judge not calibrated — {len(violations)} violation(s).**")
        lines.append("")
        for v in violations:
            lines.append(f"- {v}")
    else:
        lines.append("**Judge calibrated within policy.**")
    lines.append("")
    return "\n".join(lines)


# ----------------------------------------------------------------------------
# selftest — deterministic, no model. Asserts a FAILURE (BLOCK) case.
# ----------------------------------------------------------------------------
def _judge_interpreter() -> str:
    """A judge command shlex.split()s in run_judge with posix=False on Windows,
    which keeps quotes literally — so the interpreter token must have NO spaces.
    sys.executable lives under 'Program Files' (a space); fall back to the 8.3
    short path on Windows, else a bare 'python3'/'python' resolved via PATH."""
    exe = sys.executable or ""
    if exe and " " not in exe:
        return exe.replace("\\", "/")
    if os.name == "nt" and exe:
        try:
            import ctypes  # stdlib

            buf = ctypes.create_unicode_buffer(260)
            if ctypes.windll.kernel32.GetShortPathNameW(exe, buf, 260):
                short = buf.value
                if short and " " not in short:
                    return short.replace("\\", "/")
        except (OSError, AttributeError, ValueError):
            pass
    for cand in ("python3", "python"):
        if __import__("shutil").which(cand):
            return cand
    return "python"


def _selftest() -> int:
    rc = 0
    tmp = tempfile.mkdtemp(prefix="agenteval")
    py = _judge_interpreter()

    # A recorded fake judge: emits a fixed per-dimension score, knocked down 0.5
    # when the output carries "OFFBASE" — that lets one labels entry drive an
    # over-threshold (BLOCK) divergence deterministically with no model.
    judge_path = os.path.join(tmp, "fake_judge.py").replace("\\", "/")
    with open(judge_path, "w", encoding="utf-8") as f:
        f.write(
            "import json,sys\n"
            "p=json.load(sys.stdin)\n"
            "base={'correctness':0.92}\n"
            "s=base.get(p.get('dimension',''),0.5)\n"
            "if 'OFFBASE' in p.get('output',''): s=max(0.0,s-0.5)\n"
            "print(json.dumps({'score':s}))\n"
        )
    # No quotes: both tokens are space-free, so posix=False split yields them clean.
    judge_cmd = f"{py} {judge_path}"

    def write_result(name: str, output: str) -> str:
        path = os.path.join(tmp, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "schemaVersion": 1,
                    "caseId": "selftest-1",
                    "agent": "code-review",
                    "promptRoot": ".",
                    "harness": "fake-runner",
                    "case": {"caseId": "selftest-1", "referenceOutcome": {}},
                    "trials": [{"index": 0, "output": output, "findings": []}],
                },
                fh,
            )
        return path

    aligned_result = write_result("aligned.json", "a clean strong run")
    offbase_result = write_result("offbase.json", "OFFBASE — weak run")

    def write_labels(name: str, result_path: str, human: float) -> str:
        path = os.path.join(tmp, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "schemaVersion": 1,
                    "agent": "code-review",
                    "entries": [
                        {
                            "label": "selftest",
                            "result": result_path,
                            "trialIndex": 0,
                            "human": {"correctness": human},
                        }
                    ],
                },
                fh,
            )
        return path

    # PASS case: judge 0.92 vs human 0.95 -> divergence 0.03, well within policy.
    aligned_labels = write_labels("aligned_labels.json", aligned_result, 0.95)
    # BLOCK case: judge 0.42 (OFFBASE) vs human 0.95 -> divergence 0.53 > 0.25.
    offbase_labels = write_labels("offbase_labels.json", offbase_result, 0.95)

    def call(labels_path: str) -> int:
        return main(
            [labels_path, "--judge-cmd", judge_cmd, "--policy", "/nonexistent-use-defaults"]
        )

    # Aligned -> exit 0 (calibrated).
    if call(aligned_labels) != 0:
        print("selftest: FAIL — aligned judge/human was not reported calibrated", file=sys.stderr)
        rc = 1

    # Over-threshold -> exit 1 (BLOCK). THIS is the asserts-failure case.
    if call(offbase_labels) != 1:
        print("selftest: FAIL — over-threshold divergence did not BLOCK (expected exit 1)", file=sys.stderr)
        rc = 1

    # Empty corpus -> exit 2 (malformed).
    empty_labels = os.path.join(tmp, "empty.json")
    with open(empty_labels, "w", encoding="utf-8") as fh:
        json.dump({"agent": "code-review", "entries": []}, fh)
    if call(empty_labels) != 2:
        print("selftest: FAIL — empty corpus did not error (expected exit 2)", file=sys.stderr)
        rc = 1

    if rc == 0:
        print("agent-eval-calibrate --selftest: PASS")
    return rc


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------
def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Judge-vs-human calibration gate for the subagent eval judge."
    )
    parser.add_argument("labels", nargs="?", help="Human-labelled calibration set (labels.json).")
    parser.add_argument(
        "--policy",
        default=os.path.join("docs", "agent-eval", "calibration-policy.json"),
        help="Calibration policy JSON. Default: docs/agent-eval/calibration-policy.json.",
    )
    parser.add_argument(
        "--judge-cmd",
        default=os.environ.get("SMATCHET_AGENT_EVAL_JUDGE_CMD"),
        help="External judge command (same contract as agent-eval-score.py). "
        "Reads {case,output,dimension} JSON on stdin, prints {\"score\":0..1}. "
        "Falls back to $SMATCHET_AGENT_EVAL_JUDGE_CMD.",
    )
    parser.add_argument(
        "--markdown-only",
        action="store_true",
        help="Print the calibration report; always exit 0 (advisory mode).",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="Run the deterministic self-test (no model); exercises a BLOCK case.",
    )
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.labels:
        print("ERROR: missing <labels.json> (or pass --selftest)", file=sys.stderr)
        return 2
    if not args.judge_cmd:
        print(
            "ERROR: no --judge-cmd / $SMATCHET_AGENT_EVAL_JUDGE_CMD provided "
            "(calibration needs a judge to run)",
            file=sys.stderr,
        )
        return 2

    labels = load_json(args.labels)
    if not isinstance(labels, dict):
        print("ERROR: labels file is not a JSON object", file=sys.stderr)
        return 2
    labels_dir = os.path.dirname(os.path.abspath(args.labels))

    try:
        pairs = collect_pairs(labels, labels_dir, args.judge_cmd)
        policy = load_policy(args.policy, labels.get("agent", ""))
        mean_div, violations = evaluate(pairs, policy)
    except CalibError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(emit_markdown(labels, pairs, mean_div, violations))

    if args.markdown_only:
        return 0
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
