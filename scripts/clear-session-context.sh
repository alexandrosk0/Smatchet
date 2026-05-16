#!/usr/bin/env bash
# clear-session-context.sh — archive + truncate .session-context.md at SessionStart.
#
# Wired to the SessionStart hook in .claude/settings.json. Before resetting,
# the prior scratchpad is moved to .session-context.archive/<ts>-<sid8>.md
# whenever it carries at least one `## ` heading from a SubagentStop append
# (banner-only scratchpads are skipped — nothing worth keeping).
#
# After archival, writes a fresh banner with the session id (passed by Claude
# Code in stdin JSON, falls back to env CLAUDE_SESSION_ID, then "unknown")
# and the start timestamp.
#
# Subagents do not write to .session-context.md directly. The SubagentStop
# hook (agent-token-log.py) appends a header block when the agent's report
# carries a `## Session context append` section.
#
# Cross-session recall: see agents/_shared/skills/scratchpad-recall/SKILL.md.
#
# Silent on success; never blocks the user.

set -u

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
SCRATCHPAD="$PROJECT_DIR/.session-context.md"
ARCHIVE_DIR="$PROJECT_DIR/.session-context.archive"

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

# Archive prior scratchpad when it has at least one `## ` heading
# (which only SubagentStop appends produce — banner has no such line).
if [ -f "$SCRATCHPAD" ] && grep -qE '^## ' "$SCRATCHPAD" 2>/dev/null; then
    mkdir -p "$ARCHIVE_DIR"
    # Extract prior session id from the banner's `_Session: <id> ·` line.
    PRIOR_SID="$(sed -n 's/^_Session:[[:space:]]*\([^[:space:]·]*\).*/\1/p' \
        "$SCRATCHPAD" | head -n 1)"
    [ -z "$PRIOR_SID" ] && PRIOR_SID="unknown"
    # Short 8-char prefix keeps filenames bounded.
    SID8="$(printf '%s' "$PRIOR_SID" | cut -c1-8)"
    # Filename-safe timestamp (colons -> dashes).
    TS_FS="$(printf '%s' "$TS" | tr ':' '-')"
    ARCHIVE_PATH="$ARCHIVE_DIR/${TS_FS}-${SID8}.md"
    mv -f "$SCRATCHPAD" "$ARCHIVE_PATH" 2>/dev/null || true
fi

cat > "$SCRATCHPAD" <<EOF
# Session context

_Session: ${SESSION_ID} · started: ${TS}_
_Append-only. The SubagentStop hook (agent-token-log.py) writes one header block per subagent when its report carries \`## Session context append\`. Never edit prior entries directly; the orchestrator reads this file to seed downstream delegations. Prior sessions are archived to \`.session-context.archive/\` — recall via \`scratchpad-recall\` skill._

EOF

# --- Deferred-lint hook state: discard prior-session leftovers --------------
# .lint-queue.<pid> / .lint-queue.lock are written by the inline + drain hooks
# (lint-cpp.sh / lint-cpp-drain.sh). Any orphaned entries from a crashed
# session are stale — discard rather than re-lint at startup. .tree-dirty is
# advisory and likewise stale across sessions.
rm -f "$PROJECT_DIR"/.claude/.lint-queue.* 2>/dev/null || true
rm -f "$PROJECT_DIR/.claude/.lint-queue.lock" 2>/dev/null || true
rm -f "$PROJECT_DIR/.claude/.tree-dirty" 2>/dev/null || true

exit 0
