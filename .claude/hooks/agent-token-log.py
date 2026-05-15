#!/usr/bin/env python3
# AUTO-GENERATED MIRROR of ../../agents/_shared/token-tracking/agent-token-log.py — DO NOT EDIT.
# Run scripts/sync-agents.sh to regenerate.
"""SubagentStop hook — append one JSONL line of usage stats per subagent call.

Schema: see docs/AGENT_TOKEN_TRACKING.md § Layer B.

Output:
  - $CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl (gitignored).
  - $CLAUDE_PROJECT_DIR/.session-context.md (gitignored) — appended when the
    agent's final report carries a `## Session context append` section.
    Lifecycle: the live scratchpad is rotated to
    `.session-context.archive/<ts>-<sid8>.md` by the SessionStart hook
    (`scripts/clear-session-context.sh`); recall via the `scratchpad-recall`
    skill. See AGENTS.md § Session scratchpad protocol.

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


def _agent_version(agent_name: str, project_dir: Path) -> int:
    """Read the `version:` field from the canonical agent file's frontmatter.

    Returns 1 when the field is absent or the file cannot be read.
    """
    candidate = project_dir / "agents" / f"{agent_name}.md"
    if not candidate.is_file():
        return 1
    try:
        with candidate.open("r", encoding="utf-8") as handle:
            in_frontmatter = False
            for line in handle:
                stripped = line.strip()
                if stripped == "---":
                    if not in_frontmatter:
                        in_frontmatter = True
                        continue
                    break
                if in_frontmatter:
                    m = re.match(r"version:\s*(\d+)\s*$", stripped)
                    if m:
                        return int(m.group(1))
    except OSError:
        return 1
    return 1


def _walk_transcript(transcript_path: Path):
    """Yield each parsed JSONL object from the transcript file."""
    try:
        with transcript_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue
    except OSError as exc:
        _warn(f"transcript read failed at {transcript_path}: {exc}")


def _assistant_msg(obj: dict) -> dict | None:
    msg = obj.get("message") if isinstance(obj.get("message"), dict) else obj
    if not isinstance(msg, dict):
        return None
    if msg.get("role") != "assistant":
        return None
    return msg


def _sum_usage_and_tools(transcript_path: Path) -> tuple[dict, str, dict, list[dict]]:
    """Return (usage_dict, model_full, tool_counts, assistant_msgs).

    assistant_msgs is the ordered list of assistant message dicts; the caller
    uses the last one to infer outcome / extract scratchpad section.
    """
    sums = {"in": 0, "out": 0, "cache_create": 0, "cache_read": 0}
    model_full = ""
    tools: dict[str, int] = {}
    assistant_msgs: list[dict] = []

    for obj in _walk_transcript(transcript_path):
        msg = _assistant_msg(obj)
        if not msg:
            continue
        assistant_msgs.append(msg)
        if not model_full:
            candidate = msg.get("model") or obj.get("model")
            if candidate:
                model_full = str(candidate)
        usage = msg.get("usage")
        if isinstance(usage, dict):
            sums["in"] += int(usage.get("input_tokens") or 0)
            sums["out"] += int(usage.get("output_tokens") or 0)
            sums["cache_create"] += int(usage.get("cache_creation_input_tokens") or 0)
            sums["cache_read"] += int(usage.get("cache_read_input_tokens") or 0)
        # Count tool_use blocks in the message content.
        content = msg.get("content")
        if isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("type") == "tool_use":
                    name = str(block.get("name") or "unknown")
                    tools[name] = tools.get(name, 0) + 1
    return sums, model_full, tools, assistant_msgs


_OUTCOME_TAG = re.compile(r"^\s*##\s+Outcome:\s*(applied|halted|failed|partial|aborted)\b",
                          re.IGNORECASE | re.MULTILINE)
_HALT_HINT = re.compile(r"\b(halt(?:ed|s)?|refused|REFUSE|cannot proceed|blocked by|"
                        r"fatal error|stack trace|traceback)\b", re.IGNORECASE)
_HALT_REASON = re.compile(r"^\s*\*?\*?halt[_ ]reason\*?\*?:\s*(.+?)\s*$",
                          re.IGNORECASE | re.MULTILINE)
_SELF_IMPROV = re.compile(r"^\s*##\s+Self-improvement\s*$", re.IGNORECASE | re.MULTILINE)
_SCRATCHPAD_SECTION = re.compile(
    r"(?ms)^\s*##\s+Session context append\s*\n(.*?)(?=^\s*##\s+|\Z)"
)


def _final_text(assistant_msgs: list[dict]) -> str:
    if not assistant_msgs:
        return ""
    msg = assistant_msgs[-1]
    content = msg.get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                text = block.get("text")
                if isinstance(text, str):
                    parts.append(text)
        return "\n".join(parts)
    return ""


def _infer_outcome(final_text: str) -> tuple[str, str | None]:
    """Return (outcome, halt_reason).

    Priority:
      1. Explicit `## Outcome: applied|halted|failed|partial|aborted` line.
      2. Halt keywords near the end → `halted`.
      3. `## Self-improvement` present → `applied`.
      4. Default → `applied`.
    """
    m = _OUTCOME_TAG.search(final_text)
    if m:
        outcome = m.group(1).lower()
        reason = None
        rm = _HALT_REASON.search(final_text)
        if rm:
            reason = rm.group(1).strip()
        return outcome, reason
    if _HALT_HINT.search(final_text):
        # Pick the first matching line as the reason.
        for line in final_text.splitlines():
            if _HALT_HINT.search(line):
                return "halted", line.strip()[:200]
        return "halted", None
    if _SELF_IMPROV.search(final_text):
        return "applied", None
    return "applied", None


def _extract_scratchpad_block(final_text: str) -> str:
    """Return the body (lines) of the `## Session context append` section, stripped.
    Empty string if absent."""
    m = _SCRATCHPAD_SECTION.search(final_text)
    if not m:
        return ""
    return m.group(1).strip()


def _format_tools(tools: dict) -> str:
    if not tools:
        return ""
    parts = [f"{name}×{count}" for name, count in sorted(tools.items(), key=lambda kv: -kv[1])]
    return ", ".join(parts)


def _delegation_chain(stdin_obj: dict, project_dir: Path, session_id: str) -> list[str]:
    """Best-effort scan of the session's prior agent rows to reconstruct chain.

    Returns [<oldest-ancestor>, ..., <parent>]. Empty list when no priors found
    (this is the first subagent in the session and the orchestrator is implicit).
    """
    log_path = project_dir / ".claude" / ".agent-tokens.jsonl"
    if not log_path.is_file():
        return []
    chain: list[str] = []
    try:
        with log_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if row.get("session") != session_id:
                    continue
                agent = row.get("agent")
                if agent:
                    chain.append(str(agent))
    except OSError:
        return []
    return chain


def _append_scratchpad(project_dir: Path, agent_name: str, outcome: str, body: str) -> None:
    if not body:
        return
    scratchpad = project_dir / ".session-context.md"
    ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    header = f"\n## {agent_name} · {ts} · {outcome}\n\n"
    try:
        with scratchpad.open("a", encoding="utf-8") as handle:
            handle.write(header)
            handle.write(body.rstrip() + "\n")
    except OSError as exc:
        _warn(f"scratchpad append failed at {scratchpad}: {exc}")


def main() -> int:
    stdin_obj = _read_stdin_json()

    project_dir_str = os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    project_dir = Path(project_dir_str)
    log_path = project_dir / ".claude" / ".agent-tokens.jsonl"
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
        "outcome": "applied",
        "halt_reason": None,
        "agent_version": _agent_version(agent_name, project_dir),
        "delegation_chain": [],
        "tools_used": {},
        "tool_trace": "",
    }

    if transcript_path and transcript_path.is_file():
        usage, model_full, tools, assistant_msgs = _sum_usage_and_tools(transcript_path)
        row.update(usage)
        row["model_full"] = model_full
        row["model"] = _model_family(model_full)
        row["tools_used"] = tools
        row["tool_trace"] = _format_tools(tools)

        final_text = _final_text(assistant_msgs)
        outcome, halt_reason = _infer_outcome(final_text)
        row["outcome"] = outcome
        row["halt_reason"] = halt_reason

        # Reconstruct chain BEFORE appending today's row (so the chain reflects
        # only prior siblings/parents within the same session).
        row["delegation_chain"] = _delegation_chain(stdin_obj, project_dir, session_id)

        # Append scratchpad ONLY when the report explicitly carries the section.
        _append_scratchpad(project_dir, agent_name, outcome,
                           _extract_scratchpad_block(final_text))
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
