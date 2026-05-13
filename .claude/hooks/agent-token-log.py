#!/usr/bin/env python3
"""SubagentStop hook — append one JSONL line of usage stats per subagent call.

Schema: see docs/AGENT_TOKEN_TRACKING.md § Layer B.

Output: $CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl (gitignored).

Silent on success; warnings to stderr on parse failure. Never blocks the user.
"""

import json
import os
import re
import sys
import time
from pathlib import Path


def _warn(msg: str) -> None:
    sys.stderr.write(f"agent-token-log: {msg}\n")


def _model_family(model: str) -> str:
    """opus / sonnet / haiku from a full Claude model id."""
    lo = model.lower()
    for fam in ("opus", "sonnet", "haiku"):
        if fam in lo:
            return fam
    return model or "unknown"


def _read_stdin_json() -> dict:
    raw = sys.stdin.read()
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        _warn(f"hook stdin not valid JSON: {exc}")
        return {}


def _extract_agent_name(stdin_obj: dict) -> str:
    # Try the most plausible Claude Code field names in priority order.
    for key in ("subagent_type", "subagent_name", "agent", "agent_name"):
        val = stdin_obj.get(key)
        if val:
            return str(val)
    tool_input = stdin_obj.get("tool_input") or {}
    val = tool_input.get("subagent_type")
    if val:
        return str(val)
    return "unknown"


def _sum_usage(transcript_path: Path) -> tuple[dict, str]:
    """Return (usage_dict, model_full).

    Transcript files are JSONL. Each line is one message object; assistant
    messages carry a ``usage`` block with input_tokens / output_tokens /
    cache_creation_input_tokens / cache_read_input_tokens.
    """
    sums = {"in": 0, "out": 0, "cache_create": 0, "cache_read": 0}
    model_full = ""
    try:
        with transcript_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                msg = obj.get("message") if isinstance(obj.get("message"), dict) else obj
                if not isinstance(msg, dict):
                    continue
                if msg.get("role") != "assistant":
                    continue
                if not model_full:
                    candidate = msg.get("model") or obj.get("model")
                    if candidate:
                        model_full = str(candidate)
                usage = msg.get("usage")
                if not isinstance(usage, dict):
                    continue
                sums["in"] += int(usage.get("input_tokens") or 0)
                sums["out"] += int(usage.get("output_tokens") or 0)
                sums["cache_create"] += int(usage.get("cache_creation_input_tokens") or 0)
                sums["cache_read"] += int(usage.get("cache_read_input_tokens") or 0)
    except OSError as exc:
        _warn(f"transcript read failed at {transcript_path}: {exc}")
        return sums, model_full
    return sums, model_full


def main() -> int:
    stdin_obj = _read_stdin_json()

    project_dir = os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    log_path = Path(project_dir) / ".claude" / ".agent-tokens.jsonl"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    session_id = str(stdin_obj.get("session_id") or "unknown")
    agent_name = _extract_agent_name(stdin_obj)
    transcript_raw = stdin_obj.get("transcript_path") or ""
    transcript_path = Path(transcript_raw) if transcript_raw else None

    ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    row: dict = {
        "ts": ts,
        "session": session_id,
        "agent": agent_name,
        "model": "unknown",
        "model_full": "",
        "in": 0,
        "out": 0,
        "cache_create": 0,
        "cache_read": 0,
        "duration_ms": None,
    }

    if transcript_path and transcript_path.is_file():
        usage, model_full = _sum_usage(transcript_path)
        row.update(usage)
        row["model_full"] = model_full
        row["model"] = _model_family(model_full)
    else:
        if transcript_raw:
            _warn(f"transcript_path '{transcript_raw}' missing; logging zero-row")
        row["note"] = "no-transcript"

    try:
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    except OSError as exc:
        _warn(f"log write failed at {log_path}: {exc}")
        return 0  # never block user

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
