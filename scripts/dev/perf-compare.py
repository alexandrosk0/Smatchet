#!/usr/bin/env python3
# scripts/dev/perf-compare.py — diff a fresh perf snapshot against a baseline.
#
# Companion to scripts/dev/perf-run.sh. Reads a baseline JSON (per
# scripts/dev/perf-baseline-schema.json) and a fresh scenario.run snapshot;
# emits a markdown delta table to stdout; exits non-zero on regression
# beyond docs/perf/regression-policy.json thresholds.
#
# Harness-agnostic — pure Python 3 stdlib, no third-party deps. Required CLI
# tool per docs/backlog/agent-self-improvement/tooling.md (2026-05-20 entry).
#
# Usage:
#   python scripts/dev/perf-compare.py <baseline.json> <fresh.json>
#                                      [--policy <path>] [--markdown-only]
#                                      [--scenario-id <id>]
#
# Exit codes:
#   0 — no regression beyond policy thresholds.
#   1 — at least one threshold violated.
#   2 — usage / IO error / malformed input.

from __future__ import annotations

import argparse
import io
import json
import os
import sys
from typing import Any, Dict, List, Optional, Tuple

# Force UTF-8 on stdout — the markdown report uses the Greek delta (U+0394) and
# Windows' default cp1252 console encoding chokes on it. Wrap stdout once at
# module import so every `print(...)` downstream is safe.
if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        # Python < 3.7 or non-stdio wrappers — replace with a fresh writer.
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )


# ----------------------------------------------------------------------------
# Defaults — mirror docs/perf/regression-policy.json if --policy not supplied.
# ----------------------------------------------------------------------------
DEFAULT_POLICY: Dict[str, Any] = {
    "mean_delta_pct": 10.0,
    "p99_abs_ceiling_ms": 16.67,
    "max_abs_ceiling_ms": 50.0,
    "consecutive_run_required": 2,
    "min_baseline_calls": 10,
}


def load_json(path: str) -> Dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        sys.exit(2)
    except json.JSONDecodeError as e:
        print(f"ERROR: malformed JSON in {path}: {e}", file=sys.stderr)
        sys.exit(2)


def load_policy(path: Optional[str], scenario_id: Optional[str]) -> Dict[str, Any]:
    if path is None:
        return dict(DEFAULT_POLICY)
    raw = load_json(path)
    policy = dict(DEFAULT_POLICY)
    if isinstance(raw.get("default"), dict):
        policy.update(raw["default"])
    per_scenario = raw.get("perScenario", {}) or {}
    if scenario_id and isinstance(per_scenario.get(scenario_id), dict):
        policy.update(per_scenario[scenario_id])
    return policy


def extract_rows(blob: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Pull the per-scope rows out of either a baseline file or a fresh snapshot.

    Baseline files: rows live at top-level ``.rows``.
    scenario.run / perf.snapshot JSON envelope: rows live at ``.data.rows``.
    """
    if isinstance(blob.get("rows"), list):
        return list(blob["rows"])
    data = blob.get("data")
    if isinstance(data, dict) and isinstance(data.get("rows"), list):
        return list(data["rows"])
    return []


def index_by_name(rows: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    return {row.get("name", ""): row for row in rows if isinstance(row, dict)}


def pct_delta(base: float, fresh: float) -> Optional[float]:
    if base <= 0.0:
        return None
    return ((fresh - base) / base) * 100.0


def format_delta(base: Optional[float], fresh: Optional[float]) -> str:
    if base is None and fresh is None:
        return "—"
    if base is None:
        return f"+{fresh:.3f} (new)"
    if fresh is None:
        return f"{base:.3f} → — (gone)"
    d = pct_delta(base, fresh)
    if d is None:
        return f"{base:.3f} → {fresh:.3f}"
    sign = "+" if d >= 0 else ""
    return f"{base:.3f} → {fresh:.3f} ({sign}{d:.1f} %)"


def evaluate(
    baseline_rows: List[Dict[str, Any]],
    fresh_rows: List[Dict[str, Any]],
    policy: Dict[str, Any],
) -> Tuple[List[Dict[str, Any]], List[str]]:
    """Returns (per-row delta records, list of regression messages)."""
    base_idx = index_by_name(baseline_rows)
    fresh_idx = index_by_name(fresh_rows)
    all_names = sorted(set(base_idx.keys()) | set(fresh_idx.keys()))

    rows: List[Dict[str, Any]] = []
    regressions: List[str] = []
    min_calls = int(policy.get("min_baseline_calls", 10))
    mean_pct = float(policy.get("mean_delta_pct", 10.0))
    p99_cap = float(policy.get("p99_abs_ceiling_ms", 16.67))
    max_cap = float(policy.get("max_abs_ceiling_ms", 50.0))

    for name in all_names:
        b = base_idx.get(name)
        f = fresh_idx.get(name)
        b_total = float(b["lastTotalMs"]) if b and "lastTotalMs" in b else None
        f_total = float(f["lastTotalMs"]) if f and "lastTotalMs" in f else None
        b_p99 = float(b["p99Ms"]) if b and "p99Ms" in b else None
        f_p99 = float(f["p99Ms"]) if f and "p99Ms" in f else None
        b_max = float(b["maxMs"]) if b and "maxMs" in b else None
        f_max = float(f["maxMs"]) if f and "maxMs" in f else None
        b_calls = int(b["calls"]) if b and "calls" in b else 0

        rows.append(
            {
                "name": name,
                "lastTotalMs": format_delta(b_total, f_total),
                "p99Ms": format_delta(b_p99, f_p99),
                "maxMs": format_delta(b_max, f_max),
                "baselineCalls": b_calls,
            }
        )

        # Regression checks only fire for rows that exist in BOTH and meet min
        # call-count — single-call rows are too noisy to gate on.
        if b is None or f is None or b_calls < min_calls:
            continue

        if b_total is not None and f_total is not None:
            d = pct_delta(b_total, f_total)
            if d is not None and d > mean_pct:
                regressions.append(
                    f"{name}: lastTotalMs regressed {d:+.1f}% "
                    f"(baseline {b_total:.3f} → fresh {f_total:.3f}, policy {mean_pct:+.1f}%)"
                )

        if f_p99 is not None and f_p99 > p99_cap:
            regressions.append(
                f"{name}: p99Ms {f_p99:.3f} exceeds Pillar 1 ceiling {p99_cap:.3f}"
            )

        if f_max is not None and f_max > max_cap:
            regressions.append(
                f"{name}: maxMs {f_max:.3f} exceeds policy ceiling {max_cap:.3f}"
            )

    return rows, regressions


def emit_markdown(
    scenario_id: str,
    baseline: Dict[str, Any],
    fresh: Dict[str, Any],
    rows: List[Dict[str, Any]],
    regressions: List[str],
    policy: Dict[str, Any],
) -> str:
    lines: List[str] = []
    lines.append(f"### Perf delta — `{scenario_id}`")
    lines.append("")
    base_commit = baseline.get("captureCommit", "?")[:7]
    base_host = baseline.get("captureHost", "?")
    base_date = baseline.get("captureDate", "?")
    lines.append(
        f"- baseline: `{base_commit}` ({base_host}, captured {base_date})"
    )
    lines.append(
        f"- policy: mean Δ ≤ {policy['mean_delta_pct']:.1f} % · p99 ≤ "
        f"{policy['p99_abs_ceiling_ms']:.2f} ms · max ≤ "
        f"{policy['max_abs_ceiling_ms']:.2f} ms"
    )
    lines.append("")

    if rows:
        lines.append("| scope | lastTotalMs | p99Ms | maxMs | baseline calls |")
        lines.append("|---|---|---|---|---:|")
        for r in rows:
            lines.append(
                f"| `{r['name']}` | {r['lastTotalMs']} | {r['p99Ms']} | {r['maxMs']} | "
                f"{r['baselineCalls']} |"
            )
    else:
        lines.append("(no rows in either baseline or fresh snapshot)")

    lines.append("")
    if regressions:
        lines.append(f"**Regressions: {len(regressions)}**")
        lines.append("")
        for r in regressions:
            lines.append(f"- {r}")
    else:
        lines.append("**No regressions.**")
    lines.append("")
    return "\n".join(lines)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Compare a fresh perf snapshot against a baseline.")
    parser.add_argument("baseline", help="Path to the baseline JSON.")
    parser.add_argument("fresh", help="Path to the fresh scenario.run snapshot JSON.")
    parser.add_argument(
        "--policy",
        default=os.path.join("docs", "perf", "regression-policy.json"),
        help="Regression policy JSON. Default: docs/perf/regression-policy.json.",
    )
    parser.add_argument(
        "--markdown-only",
        action="store_true",
        help="Print only the markdown report; do not change exit code on regression.",
    )
    parser.add_argument(
        "--scenario-id",
        default=None,
        help="Scenario id (used to look up per-scenario overrides). Default: parse from baseline.",
    )
    args = parser.parse_args(argv)

    baseline = load_json(args.baseline)
    fresh = load_json(args.fresh)
    scenario_id = args.scenario_id or baseline.get("scenarioId") or "<unknown>"

    if not os.path.exists(args.policy):
        # Treat absent policy as "fall back to defaults" — useful for the first
        # run before the file lands.
        policy = dict(DEFAULT_POLICY)
    else:
        policy = load_policy(args.policy, scenario_id)

    # Per-scenario override on the baseline file itself takes precedence.
    override = baseline.get("perScenarioPolicyOverride")
    if isinstance(override, dict):
        policy.update(override)

    baseline_rows = extract_rows(baseline)
    fresh_rows = extract_rows(fresh)
    rows, regressions = evaluate(baseline_rows, fresh_rows, policy)

    print(emit_markdown(scenario_id, baseline, fresh, rows, regressions, policy))

    if args.markdown_only:
        return 0
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
