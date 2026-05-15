#!/usr/bin/env python3
"""Read .claude/.agent-tokens.jsonl and emit a session + lifetime usage report.

Usage:
  python scripts/agent-tokens-report.py            # current session only
  python scripts/agent-tokens-report.py --all      # lifetime
  python scripts/agent-tokens-report.py --since 24h  # time window

Filtering rules:
  - default: latest `session` id seen in the JSONL.
  - --all:   every row.
  - --since N{h|d|w}: rows with ts >= now - N.

Pricing table (per million tokens, USD; cutoff 2026-05) is hardcoded
at the top of the script — review quarterly. Output prints the cutoff
in the report banner so stale prices are visible.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import sys
from pathlib import Path

# --- Pricing -------------------------------------------------------------
# USD per 1M tokens. Source: Anthropic public pricing, 2026-05. Update here.
PRICING_CUTOFF = "2026-05"
PRICING = {
    # Current first-party Claude API rates.
    "opus":        {"in": 5.00,  "cache_create": 6.25,  "cache_read": 0.50,  "out": 25.00},
    "sonnet":      {"in": 3.00,  "cache_create": 3.75,  "cache_read": 0.30,  "out": 15.00},
    "haiku":       {"in": 1.00,  "cache_create": 1.25,  "cache_read": 0.10,  "out": 5.00},
    # Older models whose rates differ from the current family defaults.
    "opus_legacy": {"in": 15.00, "cache_create": 18.75, "cache_read": 1.50,  "out": 75.00},
    "haiku_3_5":   {"in": 0.80,  "cache_create": 1.00,  "cache_read": 0.08,  "out": 4.00},
    "haiku_3":     {"in": 0.25,  "cache_create": 0.30,  "cache_read": 0.03,  "out": 1.25},
}


def _parse_since(spec: str) -> _dt.timedelta:
    m = re.fullmatch(r"(\d+)([hdw])", spec.strip())
    if not m:
        raise ValueError(f"--since must be like '24h', '7d', '2w'; got '{spec}'")
    n, unit = int(m.group(1)), m.group(2)
    if unit == "h": return _dt.timedelta(hours=n)
    if unit == "d": return _dt.timedelta(days=n)
    return _dt.timedelta(weeks=n)


def _load_rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def _normalized_model_name(row: dict) -> str:
    raw = str(row.get("model_full") or row.get("model") or "unknown").lower()
    return re.sub(r"[^a-z0-9]+", "-", raw).strip("-")


def _pricing_key(row: dict) -> str:
    name = _normalized_model_name(row)
    family = str(row.get("model") or "").lower()

    if "opus" in name or family == "opus":
        # Opus 4.5+ uses the current Opus rate. Opus 4.1 / 4 / 3 keep the
        # older rate, so prefer the full model id when the transcript has it.
        if (
            re.search(r"opus-4-1(?:-|$)", name)
            or re.search(r"opus-4-2025", name)
            or re.search(r"opus-4$", name)
            or re.search(r"(?:opus-3|3-opus)(?:-|$)", name)
        ):
            return "opus_legacy"
        return "opus"

    if "sonnet" in name or family == "sonnet":
        return "sonnet"

    if "haiku" in name or family == "haiku":
        if re.search(r"(?:haiku-3-5|3-5-haiku)(?:-|$)", name):
            return "haiku_3_5"
        if re.search(r"(?:haiku-3|3-haiku)(?:-|$)", name):
            return "haiku_3"
        return "haiku"

    return family or "unknown"


def _row_cost(row: dict) -> float:
    pricing = PRICING.get(_pricing_key(row))
    if not pricing:
        return 0.0
    return (
        row.get("in", 0)            * pricing["in"]           / 1_000_000
        + row.get("cache_create", 0)* pricing["cache_create"] / 1_000_000
        + row.get("cache_read", 0)  * pricing["cache_read"]   / 1_000_000
        + row.get("out", 0)         * pricing["out"]          / 1_000_000
    )


def _fmt_k(n: int) -> str:
    if n >= 1_000_000: return f"{n/1_000_000:.1f}M"
    if n >= 1_000:     return f"{n/1_000:.1f}k"
    return str(n)


def _select_rows(rows: list[dict], mode: str, since: _dt.timedelta | None) -> list[dict]:
    if mode == "all":
        return rows
    if mode == "since":
        cutoff = _dt.datetime.now(_dt.timezone.utc).replace(tzinfo=None) - (since or _dt.timedelta(hours=24))
        out: list[dict] = []
        for r in rows:
            ts = r.get("ts")
            if not ts:
                continue
            try:
                t = _dt.datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")
            except ValueError:
                continue
            if t >= cutoff:
                out.append(r)
        return out
    # default: latest session
    if not rows:
        return []
    latest_session = rows[-1].get("session")
    return [r for r in rows if r.get("session") == latest_session]


def _aggregate(rows: list[dict]) -> dict[tuple[str, str], dict]:
    """Group by (agent, model). Sum tokens + call count + cost."""
    buckets: dict[tuple[str, str], dict] = {}
    for r in rows:
        key = (r.get("agent", "unknown"), r.get("model", "unknown"))
        b = buckets.setdefault(key, {
            "calls": 0, "in": 0, "out": 0, "cache_create": 0, "cache_read": 0, "cost": 0.0,
        })
        b["calls"]        += 1
        b["in"]           += int(r.get("in", 0))
        b["out"]          += int(r.get("out", 0))
        b["cache_create"] += int(r.get("cache_create", 0))
        b["cache_read"]   += int(r.get("cache_read", 0))
        b["cost"]         += _row_cost(r)
    return buckets


def _render(rows: list[dict], header: str) -> str:
    buckets = _aggregate(rows)
    lines: list[str] = [header, ""]
    # Header row
    lines.append(f"{'Agent':<20} {'Model':<8} {'Calls':>6}  {'In':>8}  {'Out':>8}  {'Cache':>8}  {'Est. USD':>9}")
    lines.append("-" * 78)
    total = {"calls": 0, "in": 0, "out": 0, "cache_create": 0, "cache_read": 0, "cost": 0.0}
    # Sort by descending cost
    for (agent, model), b in sorted(buckets.items(), key=lambda kv: -kv[1]["cost"]):
        cache = b["cache_create"] + b["cache_read"]
        lines.append(
            f"{agent:<20.20} {model:<8.8} {b['calls']:>6}  {_fmt_k(b['in']):>8}  {_fmt_k(b['out']):>8}  {_fmt_k(cache):>8}  ${b['cost']:>7.4f}"
        )
        for k in ("calls", "in", "out", "cache_create", "cache_read", "cost"):
            total[k] += b[k]
    lines.append("-" * 78)
    total_cache = total["cache_create"] + total["cache_read"]
    lines.append(
        f"{'Total':<20} {'':<8} {total['calls']:>6}  {_fmt_k(total['in']):>8}  {_fmt_k(total['out']):>8}  {_fmt_k(total_cache):>8}  ${total['cost']:>7.4f}"
    )
    lines.append("")

    # Outcome breakdown (new in 2026-05; older rows lack the field and count
    # as "applied" since that was the default behaviour).
    outcomes: dict[str, int] = {}
    halt_reasons: dict[str, int] = {}
    versions: dict[str, set[int]] = {}
    chain_depths: list[int] = []
    for r in rows:
        outcome = str(r.get("outcome") or "applied")
        outcomes[outcome] = outcomes.get(outcome, 0) + 1
        reason = r.get("halt_reason")
        if reason:
            halt_reasons[str(reason)] = halt_reasons.get(str(reason), 0) + 1
        ver = r.get("agent_version")
        if ver is not None:
            agent = r.get("agent", "unknown")
            versions.setdefault(agent, set()).add(int(ver))
        chain = r.get("delegation_chain") or []
        if isinstance(chain, list):
            chain_depths.append(len(chain))

    if outcomes:
        lines.append("Outcomes:")
        for k in ("applied", "halted", "failed", "partial", "aborted"):
            if outcomes.get(k):
                lines.append(f"  {k:<10} {outcomes[k]:>4}")
        # Surface anything unexpected.
        for k, v in outcomes.items():
            if k not in {"applied", "halted", "failed", "partial", "aborted"}:
                lines.append(f"  {k:<10} {v:>4}")
        lines.append("")

    if halt_reasons:
        lines.append("Top halt reasons:")
        for reason, count in sorted(halt_reasons.items(), key=lambda kv: -kv[1])[:5]:
            short = reason if len(reason) <= 70 else reason[:67] + "..."
            lines.append(f"  ({count}) {short}")
        lines.append("")

    if chain_depths:
        max_depth = max(chain_depths)
        if max_depth > 0:
            avg = sum(chain_depths) / len(chain_depths)
            lines.append(f"Delegation depth — max {max_depth}, avg {avg:.1f} (length of prior-agents chain).")
            lines.append("")

    # Surface prompt-version drift — same agent running on two versions in one window.
    drift = {a: sorted(vs) for a, vs in versions.items() if len(vs) > 1}
    if drift:
        lines.append("Agent-version drift in window:")
        for agent, vs in drift.items():
            lines.append(f"  {agent}: versions {vs}")
        lines.append("")

    lines.append(f"Pricing cutoff: {PRICING_CUTOFF} (update in scripts/agent-tokens-report.py).")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Smatchet agent token usage report.")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--all", action="store_true", help="Lifetime report (all rows).")
    group.add_argument("--since", metavar="WINDOW", help="Time window like 24h / 7d / 2w.")
    args = parser.parse_args()

    project_dir = os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    log_path = Path(project_dir) / ".claude" / ".agent-tokens.jsonl"
    rows = _load_rows(log_path)

    if not rows:
        sys.stdout.write(
            f"No agent token data yet. Log path: {log_path}\n"
            "Hint: spawn a subagent and the SubagentStop hook will write a row.\n"
        )
        return 0

    mode = "all" if args.all else "since" if args.since else "session"
    since = _parse_since(args.since) if args.since else None
    filtered = _select_rows(rows, mode, since)

    if mode == "all":
        header = f"Lifetime (all sessions, {len(filtered)} agent calls)"
    elif mode == "since":
        header = f"Last {args.since} ({len(filtered)} agent calls)"
    else:
        header = f"Session {filtered[0].get('session','?') if filtered else '—'} ({len(filtered)} agent calls)"

    sys.stdout.write(_render(filtered, header) + "\n")

    # Lifetime footer for the default session view
    if mode == "session" and rows:
        sessions = {r.get("session") for r in rows if r.get("session")}
        lifetime_cost = sum(_row_cost(r) for r in rows)
        sys.stdout.write(
            f"\nLifetime: {len(rows)} calls across {len(sessions)} sessions, ${lifetime_cost:.2f}.\n"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
