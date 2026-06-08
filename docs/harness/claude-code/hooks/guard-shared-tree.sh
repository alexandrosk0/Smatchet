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
# Sibling liveness is the shared authoritative-pid-preferred rule in
# session-registry-lib.sh (a real session pid that has exited is NOT live even
# inside the 30-min ts window — so a just-closed sibling stops blocking at once);
# legacy ppid=1 entries fall back to ts freshness exactly as before.
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

# Shared liveness lib. This hook is COPIED into .claude/hooks/ at setup, so it
# can't source a sibling — resolve via CLAUDE_PROJECT_DIR (runtime), then the git
# top-level of the hook's own location (covers the in-repo source path the bats
# suite runs). Missing lib -> fail OPEN (advisory guard never false-blocks).
_sr_lib="$PROJ/agents/scripts/core/session-registry-lib.sh"
if [ ! -f "$_sr_lib" ]; then
  _sr_top="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null)"
  [ -n "$_sr_top" ] && _sr_lib="$_sr_top/agents/scripts/core/session-registry-lib.sh"
fi
[ -f "$_sr_lib" ] || exit 0
# shellcheck source=agents/scripts/core/session-registry-lib.sh
. "$_sr_lib"

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
live="$(sr_count_live_siblings "$REGDIR" "$SID" "$NOW")"

if [ "$live" -gt 0 ]; then
  reason="${live} concurrent session(s) share this integration tree (${PROJ}); this op would change HEAD/working-tree under them. Do feature work in a worktree: pwsh scripts/dev/worktree.ps1 new <slug>. Override: SMATCHET_ALLOW_SHARED_SWITCH=1."
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$reason")"
fi
exit 0
