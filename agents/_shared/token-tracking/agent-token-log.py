#!/usr/bin/env python3
"""SubagentStop hook — append one JSONL line of usage stats per subagent call.

Schema: see docs/guides/agent-token-tracking.md § Layer B.

Output:
  - $CLAUDE_PROJECT_DIR/.claude/.agent-tokens.jsonl (gitignored).
  - $CLAUDE_PROJECT_DIR/.session-context.md (gitignored) — appended when the
    agent's final report carries a `## Session context append` section.
    Lifecycle: the live scratchpad is rotated to
    `.session-context.archive/<ts>-<sid8>.md` by the SessionStart hook
    (`agents/scripts/core/clear-session-context.sh`); recall via the `scratchpad-recall`
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


# ---------------------------------------------------------------------------
# State file — tracks how many JSONL lines of each transcript have been
# processed so each SubagentStop only accounts for the *delta* since the
# previous hook invocation, not the cumulative session total.
# ---------------------------------------------------------------------------

def _state_path(project_dir: Path) -> Path:
    return project_dir / ".claude" / ".agent-token-state.json"


def _load_state(state_file: Path) -> dict:
    if not state_file.is_file():
        return {}
    try:
        return json.loads(state_file.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _save_state(state_file: Path, state: dict) -> None:
    try:
        state_file.write_text(json.dumps(state), encoding="utf-8")
    except OSError as exc:
        _warn(f"state write failed: {exc}")


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
    for key in ("subagent_type", "subagent_name", "agent", "agent_name",
                "agent_type", "type"):
        val = stdin_obj.get(key)
        if val and str(val) not in ("unknown", "SubagentStop", "subagent_stop"):
            return str(val)
    tool_input = stdin_obj.get("tool_input") or {}
    val = tool_input.get("subagent_type") or tool_input.get("agent_type")
    if val:
        return str(val)
    # Agent name not in hook event — will be resolved from transcript delta below.
    return ""


def _agent_version(agent_name: str, project_dir: Path) -> int:
    """Read the `version:` field from the canonical agent file's frontmatter.

    Probes the post-reorg layout (agents/core, agents/project) then the legacy
    flat path. Returns 1 when the field is absent or no file is found. (The flat
    `agents/<name>.md` lookup alone was dead after the core/project split, so this
    telemetry always returned the fallback 1.)
    """
    candidate = None
    for sub in ("core", "project", ""):
        cand = project_dir / "agents" / sub / f"{agent_name}.md"
        if cand.is_file():
            candidate = cand
            break
    if candidate is None:
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


def _walk_transcript(transcript_path: Path, start_line: int = 0):
    """Yield (line_index, parsed_obj) from the transcript file.

    start_line — skip the first N non-empty JSONL lines (delta tracking).
    Yields all lines when start_line=0 (full scan for agent-name lookup).
    """
    try:
        with transcript_path.open("r", encoding="utf-8", errors="replace") as handle:
            idx = 0
            for raw in handle:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    obj = json.loads(raw)
                except json.JSONDecodeError:
                    idx += 1
                    continue
                yield idx, obj
                idx += 1
    except OSError as exc:
        _warn(f"transcript read failed at {transcript_path}: {exc}")


def _assistant_msg(obj: dict) -> dict | None:
    msg = obj.get("message") if isinstance(obj.get("message"), dict) else obj
    if not isinstance(msg, dict):
        return None
    if msg.get("role") != "assistant":
        return None
    return msg


def _extract_agent_name_from_new_lines(
    transcript_path: Path, start_line: int
) -> str:
    """Scan new transcript lines (>= start_line) for an Agent tool_use block.

    The Agent tool_use block is emitted by the orchestrator just before the
    subagent runs. Its `input.subagent_type` field names the agent type. We
    return the last match found in the delta window (most-recent agent call)
    so parallel dispatch resolves to the latest pending agent; falls back to
    "unknown".
    """
    found = ""
    for idx, obj in _walk_transcript(transcript_path):
        if idx < start_line:
            continue
        msg = _assistant_msg(obj)
        if not msg:
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for block in content:
            if (
                isinstance(block, dict)
                and block.get("type") == "tool_use"
                and block.get("name") == "Agent"
            ):
                inp = block.get("input") or {}
                subagent_type = inp.get("subagent_type") or ""
                if subagent_type:
                    found = str(subagent_type)
    return found or "unknown"


def _sum_usage_and_tools(
    transcript_path: Path, start_line: int = 0
) -> tuple[dict, str, dict, list[dict], int]:
    """Return (usage_dict, model_full, tool_counts, assistant_msgs, total_line_count).

    Only assistant messages at line index >= start_line are counted — this
    gives the per-subagent delta rather than the cumulative session total.
    total_line_count is the number of non-empty JSONL lines seen (for state).
    assistant_msgs is the ordered list of assistant message dicts from the
    delta window; the caller uses the last one to infer outcome.
    """
    sums = {"in": 0, "out": 0, "cache_create": 0, "cache_read": 0}
    model_full = ""
    tools: dict[str, int] = {}
    assistant_msgs: list[dict] = []
    total_lines = 0

    for idx, obj in _walk_transcript(transcript_path):
        total_lines = idx + 1
        if idx < start_line:
            # Still need model_full from early lines for robustness.
            if not model_full:
                msg = _assistant_msg(obj)
                if msg:
                    candidate = msg.get("model") or obj.get("model")
                    if candidate:
                        model_full = str(candidate)
            continue
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
        content = msg.get("content")
        if isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("type") == "tool_use":
                    name = str(block.get("name") or "unknown")
                    tools[name] = tools.get(name, 0) + 1
    return sums, model_full, tools, assistant_msgs, total_lines


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

    Priority (per AGENTS.md § Agent output contract):
      1. Explicit `## Outcome: applied|halted|failed|partial|aborted` line.
         This is the contract; every agent prompt mandates it.
      2. Halt keywords near the end → `halted` (defensive — agent forgot
         the tag but emitted a halt signal).
      3. Default → `partial`. An agent that did not emit `## Outcome:` and
         didn't trip a halt keyword is **not** automatically green; the
         missing tag is itself a signal that something diverged. The
         former rule ("`## Self-improvement` present → `applied`") was
         dropped because it silently green-washed halted runs — see
         docs/plans/shipped/agent-contract-alignment.md § Pre-resolved decision 5.
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
    return "partial", None


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
    # _extract_agent_name returns "" when the hook event has no name field.
    agent_name = _extract_agent_name(stdin_obj)
    transcript_raw = stdin_obj.get("transcript_path") or ""
    transcript_path = Path(transcript_raw) if transcript_raw else None

    ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    # Load transcript-position state for delta tracking.
    state_file = _state_path(project_dir)
    state = _load_state(state_file)
    transcript_key = str(transcript_path) if transcript_path else ""
    start_line = int(state.get(transcript_key, 0)) if transcript_key else 0

    row: dict = {
        "ts": ts,
        "session": session_id,
        "agent": agent_name or "unknown",
        "model": "unknown",
        "model_full": "",
        "in": 0,
        "out": 0,
        "cache_create": 0,
        "cache_read": 0,
        "duration_ms": None,
        # Default to "partial", NOT "applied": a row with no transcript / no
        # explicit `## Outcome:` tag has zero evidence of success, so green-washing
        # it as applied is wrong (matches the missing-tag inference rule in
        # docs/plans/shipped/agent-contract-alignment.md). _infer_outcome overwrites
        # this when a transcript is present.
        "outcome": "partial",
        "halt_reason": None,
        "agent_version": 1,
        "delegation_chain": [],
        "tools_used": {},
        "tool_trace": "",
    }

    if transcript_path and transcript_path.is_file():
        usage, model_full, tools, assistant_msgs, total_lines = \
            _sum_usage_and_tools(transcript_path, start_line)
        row.update(usage)
        row["model_full"] = model_full
        row["model"] = _model_family(model_full)
        row["tools_used"] = tools
        row["tool_trace"] = _format_tools(tools)

        # Resolve agent name from new transcript lines when the hook event
        # didn't supply it (current Claude Code behaviour).
        if not agent_name:
            agent_name = _extract_agent_name_from_new_lines(
                transcript_path, start_line
            )
            row["agent"] = agent_name or "unknown"

        row["agent_version"] = _agent_version(agent_name, project_dir)

        final_text = _final_text(assistant_msgs)
        outcome, halt_reason = _infer_outcome(final_text)
        row["outcome"] = outcome
        row["halt_reason"] = halt_reason

        # Reconstruct chain BEFORE appending today's row.
        row["delegation_chain"] = _delegation_chain(stdin_obj, project_dir, session_id)

        # Append scratchpad ONLY when the report explicitly carries the section.
        _append_scratchpad(project_dir, agent_name or "unknown", outcome,
                           _extract_scratchpad_block(final_text))

        # Advance the transcript position so the next SubagentStop only sees
        # the delta since this call.
        if transcript_key and total_lines > start_line:
            state[transcript_key] = total_lines
            _save_state(state_file, state)
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
