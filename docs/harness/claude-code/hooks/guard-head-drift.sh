#!/usr/bin/env bash
# guard-head-drift.sh — PreToolUse(Edit|Write|Bash) HEAD-drift guard.
#
# THE primary fix for the concurrent-session "wrong-branch commit" incident: when
# another actor (a sibling session, the user's terminal, a janitor) moves the
# shared HEAD under this session, the session's next Edit/Write/commit would land
# against the wrong branch. This hook compares live HEAD to the baseline this
# session recorded at SessionStart (sha-level, so it also catches same-branch
# pull/reset) and DENIES the write/commit. It also blocks direct commits to
# develop/main in the integration tree.
#
# Registry-INDEPENDENT of sibling liveness: it only reads THIS session's own
# baseline, so it fires even against a sibling that predates these hooks.
#
# Allow = exit 0 (no stdout). Deny = the PreToolUse permissionDecision JSON.
# Override: SMATCHET_ACK_BRANCH_DRIFT=1 (you accept the current branch).
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.

set -u

deny() {
  # %s is pre-sanitised (our own static text + git refs); keep it on one line.
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$1"
  exit 0
}

[ "${SMATCHET_ACK_BRANCH_DRIFT:-}" = "1" ] && exit 0

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"
INPUT="$(cat || true)"
[ -n "$INPUT" ] || exit 0

json_field() { # $1 = jq filter, $2 = sed key
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$INPUT" | jq -r "$1 // empty" 2>/dev/null
  else
    printf '%s' "$INPUT" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n1
  fi
}

SID="$(json_field '.session_id' 'session_id')"
[ -n "$SID" ] || SID="${CLAUDE_SESSION_ID:-}"
[ -n "$SID" ] || exit 0

ENTRY="$PROJ/.claude/.active-sessions/$SID"
[ -f "$ENTRY" ] || exit 0   # no baseline for this session -> allow (never false-block)

BASE_BRANCH="$(sed -n 's/^branch=//p' "$ENTRY" | head -n1)"
BASE_SHA="$(sed -n 's/^sha=//p' "$ENTRY" | head -n1)"

CUR_BRANCH="$(git -C "$PROJ" symbolic-ref --short HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_BRANCH" ] || exit 0   # detached / mid-rebase -> allow
CUR_SHA="$(git -C "$PROJ" rev-parse HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_SHA" ] || exit 0

# Integration tree = the main worktree (.git is a directory; a linked worktree's
# .git is a file pointer).
IS_INTEGRATION=0
[ -d "$PROJ/.git" ] && IS_INTEGRATION=1

drifted=0
[ -n "$BASE_BRANCH" ] && [ "$CUR_BRANCH" != "$BASE_BRANCH" ] && drifted=1
[ -n "$BASE_SHA" ]    && [ "$CUR_SHA" != "$BASE_SHA" ]       && drifted=1

short_base="$(printf '%s' "$BASE_SHA" | cut -c1-8)"
short_cur="$(printf '%s' "$CUR_SHA" | cut -c1-8)"

TOOL="$(json_field '.tool_name' 'tool_name')"

case "$TOOL" in
  Bash)
    CMD="$(json_field '.tool_input.command' 'command')"
    # git commit detection: `git [-C path|-c k=v|--flag]* commit`.
    if printf '%s' "$CMD" | grep -qE '(^|[;&|[:space:]])git([[:space:]]+(-C[[:space:]]+[^[:space:]]+|-c[[:space:]]+[^[:space:]]+|--[^[:space:]]+))*[[:space:]]+commit($|[[:space:]])'; then
      if [ "$IS_INTEGRATION" = "1" ] && { [ "$CUR_BRANCH" = "develop" ] || [ "$CUR_BRANCH" = "main" ]; }; then
        deny "No direct commit to ${CUR_BRANCH} in the integration tree (${PROJ}). Feature work belongs in a worktree: pwsh scripts/dev/worktree.ps1 new <slug>. Override: SMATCHET_ACK_BRANCH_DRIFT=1."
      fi
      if [ "$drifted" = "1" ]; then
        deny "HEAD moved under this session (started ${BASE_BRANCH}@${short_base}, now ${CUR_BRANCH}@${short_cur}) — about to commit to the wrong branch. Re-baseline if intended: pwsh scripts/dev/worktree.ps1 resync. Or isolate: pwsh scripts/dev/worktree.ps1 new <slug> + cherry-pick. Do NOT git switch back in a shared tree (it rug-pulls live siblings)."
      fi
    fi
    exit 0
    ;;
  Edit|Write|MultiEdit|NotebookEdit)
    if [ "$drifted" = "1" ]; then
      deny "HEAD moved under this session (started ${BASE_BRANCH}@${short_base}, now ${CUR_BRANCH}@${short_cur}). Your last Read is stale and this write would land against the wrong branch. Re-baseline if intended: pwsh scripts/dev/worktree.ps1 resync. Or isolate: pwsh scripts/dev/worktree.ps1 new <slug>."
    fi
    exit 0
    ;;
  *)
    exit 0
    ;;
esac
