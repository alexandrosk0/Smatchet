#!/usr/bin/env bash
# guard-shared-tree.sh — PreToolUse(Bash) aggressor guard (SECONDARY).
#
# In the shared integration tree only, block a HEAD/working-tree-mutating git op
# (checkout / switch / pull / reset / merge / rebase / stash pop) when ANOTHER
# session is live in that tree — such an op would rug-pull the sibling.
#
# This is advisory defense-in-depth (registry-dependent, bias-to-allow on
# uncertainty). The hard net is guard-head-drift.sh, which protects the victim
# regardless of who moved HEAD. Recovery from a drift is `worktree.ps1 resync` or
# a new worktree, NOT switching the shared HEAD back — so this guard does not
# exempt "switch back to my baseline".
#
# Allow = exit 0 (no stdout). Deny = permissionDecision JSON.
# Override: SMATCHET_ALLOW_SHARED_SWITCH=1.
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.

set -u

# Minimal JSON string escape (backslash + double-quote) so Windows paths (C:\...)
# can't produce invalid JSON that silently fails open.
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

[ "${SMATCHET_ALLOW_SHARED_SWITCH:-}" = "1" ] && exit 0

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"
# Integration tree only (main worktree has a .git directory).
[ -d "$PROJ/.git" ] || exit 0

INPUT="$(cat || true)"
[ -n "$INPUT" ] || exit 0

json_field() { # $1 = jq filter, $2 = sed key
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$INPUT" | jq -r "$1 // empty" 2>/dev/null
  else
    printf '%s' "$INPUT" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n1
  fi
}

CMD="$(json_field '.tool_input.command' 'command')"
[ -n "$CMD" ] || exit 0

# Mutating git ops only.
printf '%s' "$CMD" | grep -qE '(^|[;&|[:space:]])git([[:space:]]+(-C[[:space:]]+[^[:space:]]+|-c[[:space:]]+[^[:space:]]+|--[^[:space:]]+))*[[:space:]]+(checkout|switch|pull|reset|merge|rebase)($|[[:space:]])|(^|[;&|[:space:]])git[[:space:]]+stash[[:space:]]+pop($|[[:space:]])' || exit 0

SID="$(json_field '.session_id' 'session_id')"
[ -n "$SID" ] || SID="${CLAUDE_SESSION_ID:-}"

REGDIR="$PROJ/.claude/.active-sessions"
[ -d "$REGDIR" ] || exit 0

NOW="$(date -u +%s)"
live=0
for f in "$REGDIR"/*; do
  [ -f "$f" ] || continue
  [ -n "$SID" ] && [ "$(basename "$f")" = "$SID" ] && continue
  fts="$(sed -n 's/^ts=//p' "$f" | head -n1)"
  fpid="$(sed -n 's/^ppid=//p' "$f" | head -n1)"
  fresh=0
  case "$fts" in ''|*[!0-9]*) : ;; *) [ $((NOW - fts)) -lt 1800 ] && fresh=1 ;; esac
  alive=0
  case "$fpid" in ''|*[!0-9]*) : ;; *) kill -0 "$fpid" 2>/dev/null && alive=1 ;; esac
  { [ "$fresh" = 1 ] || [ "$alive" = 1 ]; } && live=$((live + 1))
done

if [ "$live" -gt 0 ]; then
  reason="${live} concurrent session(s) share this integration tree (${PROJ}); this op would change HEAD/working-tree under them. Do feature work in a worktree: pwsh scripts/dev/worktree.ps1 new <slug>. Override: SMATCHET_ALLOW_SHARED_SWITCH=1."
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$reason")"
fi
exit 0
