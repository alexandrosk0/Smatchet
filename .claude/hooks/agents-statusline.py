#!/usr/bin/env python3
"""Statusline badge — appends a per-session agent-token summary to whatever
the existing caveman statusline emits.

Wiring (per-user, in ~/.claude/settings.json):
    "statusLine": {
        "type": "command",
        "command": "python \"<repo>/.claude/hooks/agents-statusline.py\""
    }

If caveman is installed, this script also invokes its statusline first so
both badges appear (caveman compression badge + Smatchet agent-token badge).

Reads:
    $CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl  (tail-only; fast)

Output (single line):
    [CAVEMAN] ⛏ 12.4k    [AGENTS] 🤖 perf-detective 14k · spike-hunter 8k · total 28k · $0.42

Silent on every error path. Statusline must never fail — Claude Code refreshes
this on every interaction.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

# Force UTF-8 stdout — Windows default (cp1252) can't encode the agent emoji.
# Statusline must never crash on an encoding error.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, OSError):
    pass

# Reuse the pricing table by reading scripts/agent-tokens-report.py constants
# inline (no import — keep this standalone fast).
PRICING = {
    "opus":   {"in": 15.00, "cache_create": 18.75, "cache_read": 1.50, "out": 75.00},
    "sonnet": {"in":  3.00, "cache_create":  3.75, "cache_read": 0.30, "out": 15.00},
    "haiku":  {"in":  0.80, "cache_create":  1.00, "cache_read": 0.08, "out":  4.00},
}

MAX_TAIL_LINES = 200  # cap statusline scan; older rows just don't count for this session
TOP_N_AGENTS = 3      # show top N agents by token spend in the session


def _fmt_k(n: int) -> str:
    if n >= 1_000_000: return f"{n/1_000_000:.1f}M"
    if n >= 1_000:     return f"{n/1_000:.1f}k"
    return str(n)


def _row_cost(row: dict) -> float:
    model = row.get("model", "unknown")
    p = PRICING.get(model)
    if not p:
        return 0.0
    return (
        row.get("in", 0)            * p["in"]           / 1_000_000
        + row.get("cache_create", 0)* p["cache_create"] / 1_000_000
        + row.get("cache_read", 0)  * p["cache_read"]   / 1_000_000
        + row.get("out", 0)         * p["out"]          / 1_000_000
    )


def _caveman_badge() -> str:
    """Run the existing caveman statusline (best-effort)."""
    home = Path.home()
    candidates = [
        home / ".claude" / "hooks" / "caveman-statusline.ps1",
        home / ".claude" / "hooks" / "caveman-statusline.sh",
    ]
    for script in candidates:
        if not script.exists():
            continue
        try:
            if script.suffix == ".ps1":
                out = subprocess.run(
                    ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                     "-File", str(script)],
                    capture_output=True, text=True, timeout=2,
                )
            else:
                out = subprocess.run(
                    ["bash", str(script)],
                    capture_output=True, text=True, timeout=2,
                )
            return out.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            continue
    return ""


def _agents_badge() -> str:
    project_dir = os.environ.get("CLAUDE_PROJECT_DIR")
    if not project_dir:
        return ""
    log_path = Path(project_dir) / ".claude" / ".agent-tokens.jsonl"
    if not log_path.exists():
        return ""

    # Tail only the last MAX_TAIL_LINES rows for speed.
    try:
        with log_path.open("r", encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError:
        return ""
    lines = lines[-MAX_TAIL_LINES:]

    rows: list[dict] = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    if not rows:
        return ""

    # Latest session = most-recent row's session id.
    latest = rows[-1].get("session")
    session_rows = [r for r in rows if r.get("session") == latest]
    if not session_rows:
        return ""

    # Aggregate by agent.
    by_agent: dict[str, dict] = {}
    for r in session_rows:
        agent = r.get("agent", "unknown")
        b = by_agent.setdefault(agent, {"tokens": 0, "cost": 0.0})
        b["tokens"] += (
            int(r.get("in", 0)) + int(r.get("out", 0))
            + int(r.get("cache_create", 0)) + int(r.get("cache_read", 0))
        )
        b["cost"] += _row_cost(r)

    if not by_agent:
        return ""

    top = sorted(by_agent.items(), key=lambda kv: -kv[1]["tokens"])[:TOP_N_AGENTS]
    parts = [f"{name} {_fmt_k(b['tokens'])}" for name, b in top]
    total_tokens = sum(b["tokens"] for b in by_agent.values())
    total_cost = sum(b["cost"] for b in by_agent.values())

    return f"[AGENTS] 🤖 " + " · ".join(parts) + f" · total {_fmt_k(total_tokens)} · ${total_cost:.2f}"


def main() -> int:
    try:
        caveman = _caveman_badge()
        agents = _agents_badge()
    except Exception:
        # Statusline must never blow up. Swallow everything.
        sys.stdout.write("")
        return 0

    pieces = [p for p in (caveman, agents) if p]
    sys.stdout.write("    ".join(pieces))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        # Last-resort silent exit; statusline cannot block the shell.
        raise SystemExit(0)
