#!/usr/bin/env bash
# guard-head-drift.sh — PreToolUse(Edit|Write|Bash|PowerShell) HEAD-drift guard.
#
# THE primary fix for the concurrent-session "wrong-branch commit" incident: when
# another actor (a sibling session, the user's terminal, a janitor) moves the
# shared HEAD under this session, the session's next Edit/Write/commit would land
# against the wrong branch. This hook compares live HEAD to the baseline this
# session recorded at SessionStart (sha-level, so it also catches same-branch
# pull/reset) and DENIES the write/commit. It also blocks direct commits to
# develop/main in the integration tree.
#
# When already drifted it ALSO denies further HEAD-moving git ops (pull/reset/
# merge/rebase/checkout/switch) — recovery is `worktree.ps1 resync` or a new
# worktree, never another shared-tree move. This is what makes resync-head-
# baseline.sh safe: a HEAD-moving op only succeeds from a clean baseline, so the
# PostToolUse re-baseline can never mask a pre-existing external drift.
#
# Registry-INDEPENDENT of sibling liveness: it only reads THIS session's own
# baseline, so it fires even against a sibling that predates these hooks.
#
# Allow = exit 0 (no stdout). Deny = the PreToolUse permissionDecision JSON.
# Override: SMATCHET_ACK_BRANCH_DRIFT=1 (you accept the current branch).
#
# See docs/agent-rules/process-rules.md - Concurrent interactive sessions.

set -u

# Minimal JSON string escape (backslash + double-quote) so Windows paths (C:\...)
# and branch names with quotes can't produce invalid JSON and silently fail open.
# Reasons are single-line static text + git refs/paths, so control-char escaping
# beyond this is unnecessary.
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

deny() {
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$1")"
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

drift_reason="HEAD moved under this session (started ${BASE_BRANCH}@${short_base}, now ${CUR_BRANCH}@${short_cur}). Re-baseline if intended: pwsh scripts/dev/worktree.ps1 resync. Or isolate: pwsh scripts/dev/worktree.ps1 new <slug>. Do NOT git switch back in a shared tree (it rug-pulls live siblings)."

TOOL="$(json_field '.tool_name' 'tool_name')"

# Shared git-invocation grammar: `git [-C path|-c k=v|--flag]* <op>`.
GIT_OPTS_RE='([[:space:]]+(-C[[:space:]]+[^[:space:]]+|-c[[:space:]]+[^[:space:]]+|--[^[:space:]]+))*'

# True (0) when EVERY git invocation in $CMD matching the op alternation carries
# a `-C <path>` whose target is a linked worktree (.git is a FILE pointer, never
# the integration tree's .git directory) on a non-protected branch. Such an op
# never touches this session's tree, so the denies below don't apply — this is
# what lets an integration-tree-rooted session ship from a worktree it created
# (tooling.md P2; the PR #916/#936 friction). Any -C-less invocation, integration
# -C target, or protected-branch target keeps the deny (conservative).
all_git_ops_target_safe_worktree() { # $1 = op alternation, e.g. 'commit'
  local ops="$1" m tgt branch
  local matches
  matches="$(printf '%s' "$CMD" | grep -oE "(^|[;&|[:space:]])git${GIT_OPTS_RE}[[:space:]]+(${ops})(\$|[[:space:]])")"
  [ -n "$matches" ] || return 1
  while IFS= read -r m; do
    [ -n "$m" ] || continue
    # Last -C wins (matches git's own behaviour for repeated -C... close enough:
    # git actually chains relative -C, but absolute-path duplicates are the only
    # realistic agent shape and the LAST one is the effective base there).
    tgt="$(printf '%s' "$m" | sed -nE 's/.*-C[[:space:]]+([^[:space:]]+).*/\1/p')"
    tgt="${tgt%\"}"; tgt="${tgt#\"}"; tgt="${tgt%\'}"; tgt="${tgt#\'}"
    [ -n "$tgt" ] || return 1                       # no -C → targets this tree
    case "$tgt" in [A-Za-z]:*|/*|\\\\*) ;; *) tgt="$PROJ/$tgt" ;; esac
    [ -f "$tgt/.git" ] || return 1                  # not a linked worktree
    branch="$(git -C "$tgt" symbolic-ref --short HEAD 2>/dev/null)" || return 1
    case "$branch" in develop|main|"") return 1 ;; esac
  done <<MATCHES
$matches
MATCHES
  return 0
}

case "$TOOL" in
  Bash|PowerShell)
    CMD="$(json_field '.tool_input.command' 'command')"
    # git commit: `git [-C path|-c k=v|--flag]* commit`.
    if printf '%s' "$CMD" | grep -qE "(^|[;&|[:space:]])git${GIT_OPTS_RE}[[:space:]]+commit(\$|[[:space:]])"; then
      if [ "$IS_INTEGRATION" = "1" ] && { [ "$CUR_BRANCH" = "develop" ] || [ "$CUR_BRANCH" = "main" ]; } \
         && ! all_git_ops_target_safe_worktree 'commit'; then
        deny "No direct commit to ${CUR_BRANCH} in the integration tree (${PROJ}). Feature work belongs in a worktree: pwsh scripts/dev/worktree.ps1 new <slug> — then commit with an explicit \`git -C <worktree-path> commit\` (allowed from here). Override: SMATCHET_ACK_BRANCH_DRIFT=1 (must be exported before session launch)."
      fi
    fi
    # When already drifted, deny any further HEAD-moving git op so the PostToolUse
    # re-baseline can never lock in an external drift (recover via resync first).
    # Ops explicitly -C-targeted at a linked worktree are exempt — they cannot
    # move THIS tree's HEAD, drifted or not.
    if [ "$drifted" = "1" ] && printf '%s' "$CMD" | grep -qE "(^|[;&|[:space:]])git${GIT_OPTS_RE}[[:space:]]+(commit|pull|reset|merge|rebase|checkout|switch|cherry-pick|am|revert)(\$|[[:space:]])" \
       && ! all_git_ops_target_safe_worktree 'commit|pull|reset|merge|rebase|checkout|switch|cherry-pick|am|revert'; then
      deny "$drift_reason"
    fi
    exit 0
    ;;
  Edit|Write|MultiEdit|NotebookEdit)
    if [ "$drifted" = "1" ]; then
      deny "$drift_reason Your last Read is stale and this write would land against the wrong branch."
    fi
    exit 0
    ;;
  *)
    exit 0
    ;;
esac
