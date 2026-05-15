#!/usr/bin/env bash
# clear-session-context.sh — truncate .session-context.md at SessionStart.
#
# Wired to the SessionStart hook in .claude/settings.json. Writes a fresh
# banner with the session id (passed by Claude Code in stdin JSON, falls
# back to env CLAUDE_SESSION_ID, then "unknown") and the start timestamp.
#
# Subagents do not write to .session-context.md directly. The SubagentStop
# hook (agent-token-log.py) appends a header block when the agent's report
# carries a `## Session context append` section. This script just resets
# the scratchpad for a new session.
#
# Silent on success; never blocks the user.

set -u

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
SCRATCHPAD="$PROJECT_DIR/.session-context.md"

# Try to read session id from stdin JSON if Claude Code provides one.
SESSION_ID="${CLAUDE_SESSION_ID:-unknown}"
if [ ! -t 0 ]; then
    RAW="$(cat || true)"
    if [ -n "$RAW" ]; then
        # crude extract: "session_id":"<value>"
        EXTRACTED="$(printf '%s' "$RAW" \
            | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            | head -n 1 || true)"
        if [ -n "$EXTRACTED" ]; then
            SESSION_ID="$EXTRACTED"
        fi
    fi
fi

TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$SCRATCHPAD" <<EOF
# Session context

_Session: ${SESSION_ID} · started: ${TS}_
_Append-only. The SubagentStop hook (agent-token-log.py) writes one header block per subagent when its report carries \`## Session context append\`. Never edit prior entries directly; the orchestrator reads this file to seed downstream delegations._

EOF

exit 0
