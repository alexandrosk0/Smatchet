#!/usr/bin/env bash
# test-skill-load-log.sh — V6 telemetry verification.
#
# Covers:
#   1. PostToolUse + tool_name=Skill payload appends one line to .skill-loads.jsonl.
#   2. PreToolUse with tool_name=Skill is silently ignored (no double-log).
#   3. Non-Skill tool_name is silently ignored.
#   4. Plugin skill name (with ":") resolves skill_md_path=null + approx_tokens=0.
#   5. Project skill name resolves to .claude/skills/<name>/SKILL.md + approx_tokens > 0.
#   6. setup-harness.sh links .claude/hooks/skill-load-log.py.
#
# Auto-enrolled by scripts/dev/test-all.sh.

set -euo pipefail

command -v python >/dev/null 2>&1 || { echo "python required" >&2; exit 2; }

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"
cd "$PROJ_DIR"

HOOK="$PROJ_DIR/agents/_shared/token-tracking/skill-load-log.py"
LOG="$PROJ_DIR/.claude/.skill-loads.jsonl"
TMP_BACKUP=""

PASS=0
FAIL=0
declare -a FAILURES=()

note() { echo "[skill-load-log] $*"; }
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
nope() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1"; }

cleanup() {
    if [[ -n "$TMP_BACKUP" && -f "$TMP_BACKUP" ]]; then
        mv "$TMP_BACKUP" "$LOG" 2>/dev/null || true
    else
        rm -f "$LOG" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Preserve any real session's log; restore on exit.
if [[ -f "$LOG" ]]; then
    TMP_BACKUP="$LOG.bak.$$"
    mv "$LOG" "$TMP_BACKUP"
fi

# -------------------------------------------------------------------- Test 1
note "Test 1 — PostToolUse Skill payload appends one row"
rm -f "$LOG"
PAYLOAD='{
    "hook_event_name": "PostToolUse",
    "session_id": "test-session-1",
    "tool_name": "Skill",
    "tool_input": {"skill": "caveman:caveman-help"},
    "tool_response": {"success": true},
    "duration_ms": 17
}'
echo "$PAYLOAD" | python "$HOOK"
if [[ -f "$LOG" ]] && [[ "$(wc -l < "$LOG")" -eq 1 ]]; then
    ok "PostToolUse Skill wrote exactly 1 row"
else
    nope "PostToolUse Skill did not write 1 row (got $(wc -l < "$LOG" 2>/dev/null || echo 0))"
fi

# -------------------------------------------------------------------- Test 2
note "Test 2 — PreToolUse Skill is silently ignored"
rm -f "$LOG"
PAYLOAD_PRE='{
    "hook_event_name": "PreToolUse",
    "session_id": "test-session-2",
    "tool_name": "Skill",
    "tool_input": {"skill": "anything"}
}'
echo "$PAYLOAD_PRE" | python "$HOOK"
if [[ ! -f "$LOG" ]] || [[ "$(wc -l < "$LOG")" -eq 0 ]]; then
    ok "PreToolUse Skill produced no log row"
else
    nope "PreToolUse Skill wrote a row (expected none)"
fi

# -------------------------------------------------------------------- Test 3
note "Test 3 — non-Skill tool_name silently ignored"
rm -f "$LOG"
PAYLOAD_OTHER='{
    "hook_event_name": "PostToolUse",
    "session_id": "test-session-3",
    "tool_name": "Bash",
    "tool_input": {"command": "ls"},
    "tool_response": {"success": true}
}'
echo "$PAYLOAD_OTHER" | python "$HOOK"
if [[ ! -f "$LOG" ]] || [[ "$(wc -l < "$LOG")" -eq 0 ]]; then
    ok "non-Skill tool_name produced no log row"
else
    nope "non-Skill tool_name wrote a row (expected none)"
fi

# -------------------------------------------------------------------- Test 4
note "Test 4 — plugin-namespaced skill: skill_md_path=null, approx_tokens=0"
rm -f "$LOG"
echo "$PAYLOAD" | python "$HOOK"
RECORD="$(cat "$LOG")"
if echo "$RECORD" | grep -q '"skill": "caveman:caveman-help"'; then
    ok "plugin skill name preserved"
else
    nope "plugin skill name missing from log"
fi
if echo "$RECORD" | grep -q '"skill_md_path": null'; then
    ok "plugin skill skill_md_path is null"
else
    nope "plugin skill skill_md_path is not null"
fi
if echo "$RECORD" | grep -q '"approx_tokens": 0'; then
    ok "plugin skill approx_tokens is 0"
else
    nope "plugin skill approx_tokens is not 0"
fi

# -------------------------------------------------------------------- Test 5
note "Test 5 — project skill resolves SKILL.md path + approx_tokens > 0"
# Requires .claude/skills/perf-measure to exist (Phase 0 setup-harness).
if [[ ! -e "$PROJ_DIR/.claude/skills/perf-measure/SKILL.md" ]]; then
    note "  (running setup-harness.sh claude-code to seed perf-measure link)"
    bash "$PROJ_DIR/agents/scripts/core/setup-harness.sh" claude-code >/dev/null 2>&1 || true  # seed-only; the -e check below reports if the link is still absent
fi
if [[ -e "$PROJ_DIR/.claude/skills/perf-measure/SKILL.md" ]]; then
    rm -f "$LOG"
    PAYLOAD_PROJ='{
        "hook_event_name": "PostToolUse",
        "session_id": "test-session-5",
        "tool_name": "Skill",
        "tool_input": {"skill": "perf-measure"},
        "tool_response": {"success": true},
        "duration_ms": 42
    }'
    echo "$PAYLOAD_PROJ" | python "$HOOK"
    RECORD="$(cat "$LOG")"
    # Path uses platform separator; check substring + non-null instead of regex.
    PATH_FIELD=$(echo "$RECORD" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('skill_md_path') or '')" || true)  # malformed record -> empty, let the assertion below report
    if [[ -n "$PATH_FIELD" ]] && [[ "$PATH_FIELD" == *"perf-measure"* ]] && [[ "$PATH_FIELD" == *"SKILL.md" ]]; then
        ok "project skill skill_md_path resolved ($PATH_FIELD)"
    else
        nope "project skill skill_md_path not resolved (got: $PATH_FIELD)"
    fi
    APPROX=$(echo "$RECORD" | python -c "import json,sys; print(json.loads(sys.stdin.read())['approx_tokens'])" || echo 0)  # missing key -> 0, let the assertion below report
    if [[ "$APPROX" -gt 0 ]]; then
        ok "project skill approx_tokens > 0 (got $APPROX)"
    else
        nope "project skill approx_tokens not > 0 (got $APPROX)"
    fi
else
    nope "setup-harness did not create perf-measure skill link — cannot exercise project path"
fi

# -------------------------------------------------------------------- Test 6
note "Test 6 — setup-harness links skill-load-log.py into .claude/hooks/"
if [[ -e "$PROJ_DIR/.claude/hooks/skill-load-log.py" ]]; then
    ok ".claude/hooks/skill-load-log.py is reachable"
else
    nope ".claude/hooks/skill-load-log.py missing — setup-harness did not link"
fi

# -------------------------------------------------------------------- Report
echo
# test-all.sh aggregator parses the literal "Passed: N  Failed: M" line.
echo "Passed: $PASS  Failed: $FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo
    echo "skill-load-log failures:"
    for msg in "${FAILURES[@]}"; do echo "  - $msg"; done
    exit 1
fi
exit 0
