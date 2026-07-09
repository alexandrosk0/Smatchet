#!/usr/bin/env python3
"""cost-ceiling-check.py — advisory session token-ceiling check (AI_POLICY.md § Cost control).

Closes the charter's stated-but-unbuilt gap (AGENTIC_INFRA_AUDIT.md finding A6 /
`docs/plans/ai-control-policy.md` § Out of scope): an automated backstop for the
"escalate before unbounded runs" duty. Sums the delegated-subagent token spend
recorded by the token-tracking layer (`agents/_shared/token-tracking/agent-token-log.py`
→ `.claude/.agent-tokens.jsonl`) and, when the configured ceiling is met or
exceeded, prints an ESCALATE banner telling the orchestrator to pause and confirm
budget with the human before further delegation.

Advisory by design (always exit 0 on a ceiling hit) so a SessionStart hook never
blocks a session — the WARN-first calibration ramp every Smatchet gate starts on.
`--blocking` (exit 1 on hit) exists for a future graduation decision.

Config: `project.config.json` § governance.session_token_ceiling — integer tokens
(input+output per JSONL row; cache tokens excluded); 0 disables the check.

Usage:
  cost-ceiling-check.py [--session <sid>] [--jsonl <path>] [--config <path>] [--blocking]
  cost-ceiling-check.py --selftest

Exit: 0 = under ceiling / disabled / no data (and on hit unless --blocking);
      1 = ceiling hit with --blocking, or selftest failure; 2 = usage error.
"""
# selftest: asserts-failure

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path


def _project_dir() -> Path:
    return Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[3])


def read_ceiling(config_path: Path) -> int:
    try:
        with open(config_path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
        gov = cfg.get("governance", {}) if isinstance(cfg, dict) else {}
        gov = gov if isinstance(gov, dict) else {}
        return int(gov.get("session_token_ceiling", 0) or 0)
    except (OSError, ValueError, TypeError):
        return 0  # unreadable / malformed config: fail-open (advisory tool must not invent a ceiling)


def sum_tokens(jsonl_path: Path, session_id: str) -> int:
    """Sum input+output tokens across rows, optionally filtered to one session."""
    total = 0
    try:
        with open(jsonl_path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(row, dict):
                    continue  # non-object row (array/scalar): skip, keep fail-open
                if session_id and row.get("session_id") not in (session_id, None):
                    continue
                total += int(row.get("input_tokens", 0) or 0)
                total += int(row.get("output_tokens", 0) or 0)
    except OSError:
        return 0  # no token log yet — nothing spent
    return total


def check(ceiling: int, spent: int, blocking: bool) -> int:
    if ceiling <= 0:
        return 0
    if spent < ceiling:
        return 0
    print(
        "cost-ceiling-check: ESCALATE — delegated-subagent token spend "
        f"{spent:,} >= session ceiling {ceiling:,} "
        "(project.config.json § governance.session_token_ceiling). "
        "Per AI_POLICY.md § Cost control: pause before further delegation and "
        "confirm budget with the human (AskUserQuestion), or raise/disable the "
        "ceiling deliberately."
    )
    return 1 if blocking else 0


def run_selftest() -> int:
    failures = []

    def expect(name, got, want):
        if got != want:
            failures.append(f"{name}: got {got!r}, want {want!r}")

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        jsonl = tdp / "tokens.jsonl"
        rows = [
            {"session_id": "s1", "input_tokens": 600, "output_tokens": 400},
            {"session_id": "s1", "input_tokens": 300, "output_tokens": 200,
             "cache_read_input_tokens": 999999},  # cache fields must not count
            {"session_id": "s2", "input_tokens": 5000, "output_tokens": 5000},
        ]
        # Include non-object JSONL rows (array + scalar) — must be skipped, not crash.
        jsonl.write_text(
            "\n".join(json.dumps(r) for r in rows) + "\nnot-json\n[1,2,3]\n42\n", encoding="utf-8"
        )

        expect("sum all sessions", sum_tokens(jsonl, ""), 11500)
        expect("sum one session", sum_tokens(jsonl, "s1"), 1500)
        expect("missing file is zero", sum_tokens(tdp / "absent.jsonl", ""), 0)

        cfg = tdp / "cfg.json"
        cfg.write_text(json.dumps({"governance": {"session_token_ceiling": 1000}}), encoding="utf-8")
        expect("read ceiling", read_ceiling(cfg), 1000)
        cfg.write_text(json.dumps({"governance": {}}), encoding="utf-8")
        expect("absent knob disables", read_ceiling(cfg), 0)
        expect("unreadable config disables", read_ceiling(tdp / "absent.json"), 0)
        # Malformed config shapes must fail open (0), not raise AttributeError.
        cfg.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
        expect("non-dict config disables", read_ceiling(cfg), 0)
        cfg.write_text(json.dumps({"governance": "oops"}), encoding="utf-8")
        expect("non-dict governance disables", read_ceiling(cfg), 0)

        expect("under ceiling passes", check(2000, 1500, blocking=False), 0)
        expect("disabled passes", check(0, 10**9, blocking=False), 0)
        expect("hit is advisory by default", check(1000, 1500, blocking=False), 0)
        expect("hit blocks with --blocking", check(1000, 1500, blocking=True), 1)

    if failures:
        for f in failures:
            print(f"cost-ceiling-check --selftest FAIL: {f}", file=sys.stderr)
        return 1
    print("cost-ceiling-check --selftest: all assertions passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--session", default="", help="sum only rows with this session_id")
    ap.add_argument("--jsonl", default="", help="token JSONL path override")
    ap.add_argument("--config", default="", help="project.config.json path override")
    ap.add_argument("--blocking", action="store_true", help="exit 1 on ceiling hit")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    proj = _project_dir()
    config = Path(args.config) if args.config else proj / "project.config.json"
    jsonl = Path(args.jsonl) if args.jsonl else proj / ".claude" / ".agent-tokens.jsonl"
    ceiling = read_ceiling(config)
    spent = sum_tokens(jsonl, args.session)
    return check(ceiling, spent, args.blocking)


if __name__ == "__main__":
    sys.exit(main())
