#!/usr/bin/env python3
"""PostToolUse:Skill hook — append one JSONL line per skill invocation.

Smatchet V6 telemetry (see docs/self-improvement/categories/tooling.md
2026-05-19 "Token telemetry does not record Claude Code skill-load overhead"
entry — archived to applied.md once this lands).

Why exists: Claude Code's SubagentStop hook captures token usage per subagent
spawn, but skills load **inline** in the orchestrator's context. No SubagentStop
fires for skill invocations. Without a separate hook the central claim of the
2026-05-19 perf-instrument / perf-measure dual-publish ("skill form is lighter
than agent form") is unmeasurable.

Output: $CLAUDE_PROJECT_DIR/.claude/.skill-loads.jsonl (gitignored).
One line per invocation:
  {
    "ts": <epoch seconds>,
    "session": <session_id>,
    "skill": <skill name, e.g. "caveman:caveman-help" or "perf-measure">,
    "duration_ms": <PostToolUse-reported skill body load duration>,
    "success": <bool from tool_response.success>,
    "approx_tokens": <SKILL.md byte size // 4>,  // crude heuristic; Claude
                                                  // does not expose real cost
    "skill_md_path": <resolved path used for approx_tokens, or null>,
  }

Limitations: `approx_tokens` is bytes/4 of the linked SKILL.md file — a
classical "1 token ~ 4 chars" approximation. Real tokenization differs (BPE,
Unicode, code blocks weigh differently). Use it for relative comparisons
(skill A vs skill B), not absolute budget accounting.

Silent on success; warnings to stderr on parse failure. Never blocks the user.
"""

import json
import os
import sys
import time
from pathlib import Path


def _warn(msg: str) -> None:
    sys.stderr.write(f"skill-load-log: {msg}\n")


def _read_stdin_json() -> dict:
    raw = sys.stdin.read()
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        _warn(f"hook stdin not valid JSON: {exc}")
        return {}


def _resolve_skill_md(skill_name: str, project_dir: Path) -> Path | None:
    """Locate SKILL.md for a given skill identifier.

    Skill names in the wild:
      - "perf-measure"                   (project skill, .claude/skills/perf-measure/SKILL.md)
      - "caveman:caveman-help"           (plugin skill, namespace:name)
      - "anthropic-skills:pdf"           (plugin skill)

    For project skills we follow the .claude/skills/<name>/SKILL.md path.
    For plugin skills we cannot resolve a file path from here without
    plugin-cache enumeration (paths vary by user/install). Return None.
    """
    if not skill_name or ":" in skill_name:
        return None
    candidate = project_dir / ".claude" / "skills" / skill_name / "SKILL.md"
    if candidate.is_file():
        return candidate
    return None


def _approx_tokens(path: Path | None) -> int:
    if path is None:
        return 0
    try:
        return path.stat().st_size // 4
    except OSError:
        return 0


def main() -> int:
    stdin_obj = _read_stdin_json()

    # We accept both PreToolUse and PostToolUse invocations. PostToolUse is the
    # canonical event (carries duration_ms + success); PreToolUse is silently
    # ignored to avoid double-logging if the user wires both. Distinguish by
    # `hook_event_name`.
    if stdin_obj.get("hook_event_name") == "PreToolUse":
        return 0

    if stdin_obj.get("tool_name") != "Skill":
        # Defensive: matcher should already filter, but a stray invocation
        # against the wrong tool should not log noise.
        return 0

    tool_input = stdin_obj.get("tool_input") or {}
    tool_response = stdin_obj.get("tool_response") or {}
    skill_name = str(tool_input.get("skill") or "unknown")

    project_dir_str = os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    project_dir = Path(project_dir_str)
    log_path = project_dir / ".claude" / ".skill-loads.jsonl"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    skill_md = _resolve_skill_md(skill_name, project_dir)

    record = {
        "ts": time.time(),
        "session": stdin_obj.get("session_id"),
        "skill": skill_name,
        "duration_ms": stdin_obj.get("duration_ms"),
        "success": bool(tool_response.get("success", True)),
        "approx_tokens": _approx_tokens(skill_md),
        "skill_md_path": str(skill_md) if skill_md is not None else None,
    }

    try:
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record) + "\n")
    except OSError as exc:
        _warn(f"log append failed at {log_path}: {exc}")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
