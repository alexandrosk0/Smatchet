#!/usr/bin/env python3
# scripts/dev/perf-compare.py — diff a fresh perf snapshot against a baseline.
#
# Companion to scripts/dev/perf-run.sh. Reads a baseline JSON (per
# scripts/dev/perf-baseline-schema.json) and a fresh scenario.run snapshot;
# emits a markdown delta table to stdout; exits non-zero on regression
# beyond docs/perf/regression-policy.json thresholds.
#
# Harness-agnostic — pure Python 3 stdlib, no third-party deps. Required CLI
# tool per docs/self-improvement/categories/tooling.md (2026-05-20 entry).
#
# Usage:
#   python scripts/dev/perf-compare.py <baseline.json> <fresh.json>
#                                      [--policy <path>]
#                                      [--markdown-only | --summary-only]
#                                      [--scenario-id <id>]
#
# --markdown-only emits the full per-scope delta table (for the Step Summary);
# --summary-only emits the compact per-scenario verdict + regressing rows only
# (for the PR comment). Both suppress the regression exit code.
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
# `consecutive_run_required` removed (CodeRabbit PR #321 #4): the knob
# was declared but never enforced by `evaluate()` — a single-run gate is
# what actually ships in perf-pr-fast.yml. Multi-run streak tracking
# requires the driver to retain per-row state across compare invocations,
# which is a follow-up plan. The dead knob is removed from the default
# AND from docs/perf/regression-policy.json so the schema doesn't lie
# about what the gate enforces.
DEFAULT_POLICY: Dict[str, Any] = {
    "mean_delta_pct": 10.0,
    "p99_abs_ceiling_ms": 10.0,
    "max_abs_ceiling_ms": 50.0,
    "min_baseline_calls": 10,
    # Pillar 1 absolute mean budget (6.94 ms = 1000/144) gates on a row's
    # avgPerCallMs when set. Ships DISABLED (None) until the calibration pass
    # of docs/plans/perf-gate-revival.md step 5 observes real CI runs —
    # a blanket 6.94 would be perpetually red for legitimately-heavy scopes
    # (e.g. SmatchetUI::Draw) on CI hardware; enable via regression-policy.json
    # default + perScenario overrides once baselines exist.
    "mean_abs_ceiling_ms": None,
    # Noise floor for the RELATIVE lastTotalMs gate: a relative regression only
    # fires when the absolute delta also exceeds this. Calibrated from the
    # first armed CI run (2026-06-07): ai_chat.history.render_turn flagged
    # +12.0% on 0.023 → 0.026 ms — 3 µs of single-frame sampling noise on a
    # microsecond-scale scope. 0.05 ms = 0.7 % of the 6.94 ms frame budget;
    # a regression that matters moves a scope by more than that.
    "mean_min_abs_delta_ms": 0.05,
    # RELATIVE p99 gate (Cluster-C ci-falsepositive-hardening; infra.md
    # p99-gate-warmup-frame-exclusion follow-up). The ABSOLUTE p99 ceiling
    # (p99_abs_ceiling_ms) catches a blowup but not a steady-state p99 that
    # creeps up while staying under 10 ms. This relative gate closes that gap:
    # it fires when a scope's p99 regresses by more than p99_rel_pct AND the
    # absolute delta exceeds p99_min_abs_delta_ms. Ships DISABLED (None) — same
    # posture mean_abs_ceiling_ms shipped under — because the relative form does
    # NOT cancel the CI runner's p99 noise band (common-mode cancellation only
    # helps a systematic offset, not a hot-job draw), so p99_min_abs_delta_ms
    # must be armed ABOVE the observed runner band from real CI runs. Arming a
    # live merge gate is a human-judgment call gated on observed-run evidence +
    # sign-off, never an autonomous flip — set both knobs in regression-policy.json.
    "p99_rel_pct": None,
    "p99_min_abs_delta_ms": 0.5,
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


def extract_rows(blob: Dict[str, Any], source_label: str = "<unknown>") -> List[Dict[str, Any]]:
    """Pull the per-scope rows out of either a baseline file or a fresh snapshot.

    Baseline files: rows live at top-level ``.rows``.
    scenario.run / perf.snapshot JSON envelope: rows live at ``.data.rows``.

    Raises ValueError if no `rows` list can be located. The previous
    fall-open behaviour (return []) let malformed inputs sneak past the
    gate as "no regressions found" (CodeRabbit PR #321 #5).
    """
    if isinstance(blob, dict):
        if isinstance(blob.get("rows"), list):
            return list(blob["rows"])
        data = blob.get("data")
        if isinstance(data, dict) and isinstance(data.get("rows"), list):
            return list(data["rows"])
    raise ValueError(f"missing or invalid 'rows' in {source_label}")


def _to_float(value: Any, label: str) -> Optional[float]:
    """Coerce a JSON value to float; ValueError out for the caller to catch.
    Returns None when the value is missing (None / absent), so the
    per-row metric merging logic still works when a row only carries a
    subset of fields. (CodeRabbit PR #322 #5.)
    """
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        raise ValueError(f"invalid numeric field: {label}={value!r}")


def _to_int(value: Any, label: str) -> int:
    if value is None:
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        raise ValueError(f"invalid integer field: {label}={value!r}")


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
    # All numeric policy + per-row casts go through _to_float / _to_int so
    # malformed input (string-where-number, None where number) raises
    # ValueError cleanly, which main() catches → exit 2. Previously raw
    # float()/int() casts crashed with KeyError or TypeError and bypassed
    # the documented exit-2 input-error path. (CodeRabbit PR #322 #5.)
    min_calls = _to_int(policy.get("min_baseline_calls", 10), "policy.min_baseline_calls")

    # Substitute the default only for an absent/null knob — NOT for an explicit 0.
    # `X or DEFAULT` treated `mean_delta_pct: 0` (gate on ANY regression) as falsy
    # and silently loosened it to the permissive default. `.get(key, default)`
    # already covers absence; the None-check only rescues an explicit JSON null.
    def _knob(key, default):
        v = _to_float(policy.get(key, default), "policy.%s" % key)
        return default if v is None else v

    mean_pct = _knob("mean_delta_pct", 10.0)
    p99_cap = _knob("p99_abs_ceiling_ms", 10.0)
    max_cap = _knob("max_abs_ceiling_ms", 50.0)
    # None ⇒ knob disabled (ships off until perf-gate-revival step-5 calibration).
    mean_cap = _to_float(policy.get("mean_abs_ceiling_ms"), "policy.mean_abs_ceiling_ms")
    min_abs_delta = _to_float(
        policy.get("mean_min_abs_delta_ms", 0.05), "policy.mean_min_abs_delta_ms"
    ) or 0.0
    # RELATIVE p99 gate — None ⇒ disabled (ships off until CI-run calibration
    # arms it above the runner p99 noise band; see DEFAULT_POLICY note).
    p99_rel_pct = _to_float(policy.get("p99_rel_pct"), "policy.p99_rel_pct")
    p99_min_abs_delta = _to_float(
        policy.get("p99_min_abs_delta_ms", 0.5), "policy.p99_min_abs_delta_ms"
    ) or 0.0

    for name in all_names:
        b = base_idx.get(name)
        f = fresh_idx.get(name)
        b_total = _to_float(b.get("lastTotalMs"), f"baseline.{name}.lastTotalMs") if b else None
        f_total = _to_float(f.get("lastTotalMs"), f"fresh.{name}.lastTotalMs") if f else None
        b_p99 = _to_float(b.get("p99Ms"), f"baseline.{name}.p99Ms") if b else None
        f_p99 = _to_float(f.get("p99Ms"), f"fresh.{name}.p99Ms") if f else None
        b_max = _to_float(b.get("maxMs"), f"baseline.{name}.maxMs") if b else None
        f_max = _to_float(f.get("maxMs"), f"fresh.{name}.maxMs") if f else None
        b_avg = _to_float(b.get("avgPerCallMs"), f"baseline.{name}.avgPerCallMs") if b else None
        f_avg = _to_float(f.get("avgPerCallMs"), f"fresh.{name}.avgPerCallMs") if f else None
        b_calls = _to_int(b.get("calls"), f"baseline.{name}.calls") if b else 0

        # Annotate rows the RELATIVE gate skips as below-floor noise, so the
        # table's eye-catching percentages (+3575 % on a 1-sample scope) read
        # as ungated sampling noise instead of a suspected regression
        # (categories/tooling 2026-07-05 legibility entry; PR #1632).
        floor_note = ""
        if b is not None and f is not None:
            if b_calls < min_calls:
                floor_note = f"noise: {b_calls} < {min_calls} calls"
            elif (
                b_total is not None
                and f_total is not None
                and abs(f_total - b_total) <= min_abs_delta
            ):
                # No abs-value bars — a literal `|` splits the markdown table cell.
                floor_note = f"noise: abs Δ ≤ {min_abs_delta:.3f} ms"

        rows.append(
            {
                "name": name,
                "lastTotalMs": format_delta(b_total, f_total),
                "avgPerCallMs": format_delta(b_avg, f_avg),
                "p99Ms": format_delta(b_p99, f_p99),
                "maxMs": format_delta(b_max, f_max),
                "baselineCalls": b_calls,
                "floorNote": floor_note,
            }
        )

        # Regression checks only fire for rows that exist in BOTH snapshots
        # (fresh-only rows skip gating to keep scope-bootstrap PRs quiet).
        if b is None or f is None:
            continue

        # ABSOLUTE ceilings fire regardless of baseline call count — the
        # min_baseline_calls filter below exists for the noisy RELATIVE delta
        # comparison, never for hard Pillar-1 budgets. The once-per-frame
        # umbrella scope (SmatchetUI::Draw, calls=1) is exactly the row whose
        # per-call p99 ≈ frame p99 that the 10.0 ms floor targets; filtering
        # it out made the p99/max ceilings structurally unable to fire
        # (code-review sweep CR-949-1, 2026-06-07).
        if f_p99 is not None and f_p99 > p99_cap:
            regressions.append(
                f"{name}: p99Ms {f_p99:.3f} exceeds Pillar 1 ceiling {p99_cap:.3f}"
            )

        # RELATIVE p99 gate (disabled until p99_rel_pct is armed). Catches a
        # steady-state p99 creep that stays under the absolute ceiling. The
        # abs-Δ floor (p99_min_abs_delta_ms) is the noise guard — it must be
        # armed above the runner p99 band, which the relative form does NOT
        # cancel. Independent of the min_baseline_calls floor: p99 is a
        # frame-level percentile, not a per-call sample, so a low `calls` scope
        # (e.g. the SmatchetUI::Draw umbrella) still carries a meaningful p99.
        if (
            p99_rel_pct is not None
            and b_p99 is not None
            and f_p99 is not None
        ):
            dp99 = pct_delta(b_p99, f_p99)
            if (
                dp99 is not None
                and dp99 > p99_rel_pct
                and (f_p99 - b_p99) > p99_min_abs_delta
            ):
                regressions.append(
                    f"{name}: p99Ms regressed {dp99:+.1f}% "
                    f"(baseline {b_p99:.3f} → fresh {f_p99:.3f}, policy {p99_rel_pct:+.1f}% "
                    f"and Δ > {p99_min_abs_delta:.3f} ms)"
                )

        # Pillar 1 absolute mean budget (perf-gate-revival step 5). Disabled
        # while mean_abs_ceiling_ms is null — enabled per-policy after the
        # baseline-calibration pass.
        if mean_cap is not None and f_avg is not None and f_avg > mean_cap:
            regressions.append(
                f"{name}: avgPerCallMs {f_avg:.3f} exceeds Pillar 1 mean budget {mean_cap:.3f}"
            )

        if f_max is not None and f_max > max_cap:
            regressions.append(
                f"{name}: maxMs {f_max:.3f} exceeds policy ceiling {max_cap:.3f}"
            )

        # RELATIVE gate keeps the call-count floor — single-call rows are too
        # noisy for a percentage comparison.
        if b_calls < min_calls:
            continue

        if b_total is not None and f_total is not None:
            d = pct_delta(b_total, f_total)
            # Relative gate + absolute noise floor: a +12 % swing on a
            # microsecond-scale scope is sampling noise, not a regression
            # (first armed-run calibration, perf-gate-revival step 5).
            if (
                d is not None
                and d > mean_pct
                and (f_total - b_total) > min_abs_delta
            ):
                regressions.append(
                    f"{name}: lastTotalMs regressed {d:+.1f}% "
                    f"(baseline {b_total:.3f} → fresh {f_total:.3f}, policy {mean_pct:+.1f}% "
                    f"and Δ > {min_abs_delta:.3f} ms)"
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
    # Render from normalized values — raw policy entries may be null/strings
    # (evaluate() normalizes transiently via _to_float; mirror that here so a
    # malformed policy can't TypeError the report). (CodeRabbit PR #937 #1.)
    mean_pct = _to_float(policy.get("mean_delta_pct", 10.0), "policy.mean_delta_pct") or 10.0
    p99_cap = _to_float(policy.get("p99_abs_ceiling_ms", 10.0), "policy.p99_abs_ceiling_ms") or 10.0
    max_cap = _to_float(policy.get("max_abs_ceiling_ms", 50.0), "policy.max_abs_ceiling_ms") or 50.0
    mean_cap = _to_float(policy.get("mean_abs_ceiling_ms"), "policy.mean_abs_ceiling_ms")
    p99_rel_pct = _to_float(policy.get("p99_rel_pct"), "policy.p99_rel_pct")
    policy_line = (
        f"- policy: mean Δ ≤ {mean_pct:.1f} % · p99 ≤ "
        f"{p99_cap:.2f} ms · max ≤ "
        f"{max_cap:.2f} ms"
    )
    if mean_cap is not None:
        policy_line += f" · mean ≤ {mean_cap:.2f} ms"
    if p99_rel_pct is not None:
        policy_line += f" · p99 Δ ≤ {p99_rel_pct:.1f} %"
    lines.append(policy_line)
    lines.append("")

    if rows:
        lines.append("| scope | lastTotalMs | avgPerCallMs | p99Ms | maxMs | baseline calls |")
        lines.append("|---|---|---|---|---|---:|")
        # Gated-eligible rows first; below-floor rows sink with their marker.
        for r in sorted(rows, key=lambda r: bool(r.get("floorNote"))):
            note = r.get("floorNote", "")
            total = f"{r['lastTotalMs']} · ({note})" if note else r["lastTotalMs"]
            lines.append(
                f"| `{r['name']}` | {total} | {r['avgPerCallMs']} | {r['p99Ms']} | "
                f"{r['maxMs']} | {r['baselineCalls']} |"
            )
        if any(r.get("floorNote") for r in rows):
            lines.append("")
            lines.append(
                "_Rows marked `· (noise: …)` sit below the sample/noise floor — "
                "the relative gate skips them; their percentages are ungated "
                "sampling noise, not regressions._"
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


def emit_summary(scenario_id: str, regressions: List[str]) -> str:
    """Compact per-scenario verdict for the PR comment (no per-scope table).

    The full per-scope delta table is enormous (one row per profiled scope ×
    every scenario) and already lands in the run's Step Summary. The PR thread
    only needs the verdict + the regressing rows, so a clean scenario collapses
    to a single ✅ line and a regressing one lists just its offending scopes.
    (Anti-spam comment policy, perf-pr-fast.yml § Publish perf report.)
    """
    lines: List[str] = []
    if regressions:
        lines.append(f"### `{scenario_id}` — ❌ {len(regressions)} regression(s)")
        lines.append("")
        for r in regressions:
            lines.append(f"- {r}")
    else:
        lines.append(f"### `{scenario_id}` — ✅ within policy")
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
    # --markdown-only and --summary-only both suppress the gate exit code and
    # pick a different render; selecting both is a caller bug (which render
    # wins is non-obvious). argparse rejects the combination. (CodeRabbit
    # PR #1097 #2.)
    output_mode = parser.add_mutually_exclusive_group()
    output_mode.add_argument(
        "--markdown-only",
        action="store_true",
        help="Print only the markdown report; do not change exit code on regression.",
    )
    output_mode.add_argument(
        "--summary-only",
        action="store_true",
        help="Print only the compact per-scenario verdict + regressing rows "
        "(no per-scope table); do not change exit code on regression. For the "
        "PR comment — the full table lives in the run's Step Summary.",
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

    # extract_rows + the _to_float / _to_int casts in evaluate() raise
    # ValueError on malformed input. We catch here and exit 2 — the
    # canonical "input error" exit code (1 = regression detected, 2 =
    # malformed input, 0 = clean). (CodeRabbit PR #321 #5 + PR #322 #5.)
    try:
        baseline_rows = extract_rows(baseline, args.baseline)
        fresh_rows = extract_rows(fresh, args.fresh)
        rows, regressions = evaluate(baseline_rows, fresh_rows, policy)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.summary_only:
        # Compact verdict for the PR comment; never gates (like --markdown-only).
        print(emit_summary(scenario_id, regressions))
        return 0

    print(emit_markdown(scenario_id, baseline, fresh, rows, regressions, policy))

    if args.markdown_only:
        return 0
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
