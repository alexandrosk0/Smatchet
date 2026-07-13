#!/usr/bin/env bash
# session-heartbeat.sh — Stop hook: refresh this session's registry timestamp.
#
# Stop fires at the END OF EACH TURN (not session end — there is no SessionEnd
# hook). So this is a per-turn liveness heartbeat: it updates ONLY the `ts` field
# so sibling-liveness checks (guard-shared-tree.sh, the banner) can tell a session
# is still active. It must NEVER touch `branch`/`sha` — re-baselining here would
# mask a HEAD drift that happened during the turn.
#
# Cleanup is intentionally NOT done here (deleting on Stop would drop the baseline
# after turn 1). Dead entries are swept lazily by session-tree-banner.sh (>7d).
#
# exit 0 always.

set -euo pipefail

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"

SESSION_ID="${CLAUDE_SESSION_ID:-}"
if [ ! -t 0 ]; then
  RAW="$(cat || true)"
  if [ -n "$RAW" ]; then
    EXTRACTED="$(printf '%s' "$RAW" \
      | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1 || true)"  # head can SIGPIPE sed on a multi-match payload; must-not-block hook
    [ -n "$EXTRACTED" ] && SESSION_ID="$EXTRACTED"
  fi
fi
[ -n "$SESSION_ID" ] || exit 0

ENTRY="$PROJ/.claude/.active-sessions/$SESSION_ID"
[ -f "$ENTRY" ] || exit 0

NOW="$(date -u +%s)"
if grep -q '^ts=' "$ENTRY" 2>/dev/null; then
  tmp="$(mktemp "${ENTRY}.XXXXXX")" || exit 0
  if sed "s/^ts=.*/ts=$NOW/" "$ENTRY" > "$tmp" 2>/dev/null; then
    mv -f "$tmp" "$ENTRY" 2>/dev/null || rm -f "$tmp"
  else
    rm -f "$tmp"
  fi
else
  printf 'ts=%s\n' "$NOW" >> "$ENTRY" 2>/dev/null || true
fi

exit 0
