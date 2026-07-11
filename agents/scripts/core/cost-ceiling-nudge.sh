#!/usr/bin/env bash
# cost-ceiling-nudge.sh — SessionStart hook wrapper for cost-ceiling-check.py
# (AI_POLICY.md § Cost control). Extracts the session id from the hook's stdin
# JSON (same crude pattern as clear-session-context.sh) and runs the advisory
# ceiling check against the token-tracking JSONL. Never blocks a session: the
# check is advisory (exit 0 on hit) and this wrapper fails open on any error.
set -u

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"

SESSION_ID="${CLAUDE_SESSION_ID:-}"
if [ ! -t 0 ]; then
    RAW="$(cat || true)"
    if [ -n "$RAW" ]; then
        EXTRACTED="$(printf '%s' "$RAW" \
            | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            | head -n 1 || true)"
        if [ -n "$EXTRACTED" ]; then
            SESSION_ID="$EXTRACTED"
        fi
    fi
fi

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && exit 0 # no python: fail open, never block SessionStart

"$PY" "$PROJECT_DIR/agents/scripts/core/cost-ceiling-check.py" --session "$SESSION_ID" || true
exit 0
