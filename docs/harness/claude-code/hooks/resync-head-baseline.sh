#!/usr/bin/env bash
# resync-head-baseline.sh — PostToolUse(Bash): re-baseline this session's HEAD
# registry entry AFTER a HEAD-moving git op the session itself ran (commit / pull
# / reset / merge / rebase / checkout / switch / cherry-pick / am / revert).
#
# Without this, guard-head-drift.sh would false-deny the session's own legitimate
# moves (e.g. your own commit advances the sha). It re-syncs ONLY after a matched
# move command — never after an arbitrary Bash call (so a plain `git status` / `ls`
# right after an EXTERNAL drift cannot mask it).
#
# Safe against drift-masking: guard-head-drift.sh DENIES any HEAD-moving git op
# while the session is already drifted, so a matched move command only ever
# succeeds from a clean baseline; this re-baseline therefore can never lock in a
# pre-existing external drift.
#
# exit 0 always.
set -u

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

CMD="$(json_field '.tool_input.command' 'command')"
[ -n "$CMD" ] || exit 0

# Only after a HEAD-moving git subcommand.
printf '%s' "$CMD" | grep -qE '(^|[;&|[:space:]])git([[:space:]]+(-C[[:space:]]+[^[:space:]]+|-c[[:space:]]+[^[:space:]]+|--[^[:space:]]+))*[[:space:]]+(commit|pull|reset|merge|rebase|checkout|switch|cherry-pick|am|revert)($|[[:space:]])' || exit 0

ENTRY="$PROJ/.claude/.active-sessions/$SID"
[ -f "$ENTRY" ] || exit 0

CUR_BRANCH="$(git -C "$PROJ" symbolic-ref --short HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_BRANCH" ] || exit 0
CUR_SHA="$(git -C "$PROJ" rev-parse HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_SHA" ] || exit 0

PPID_OLD="$(sed -n 's/^ppid=//p' "$ENTRY" | head -n1)"
case "$PPID_OLD" in ''|*[!0-9]*) PPID_OLD=0 ;; esac
NOW="$(date -u +%s)"

{
  printf 'branch=%s\n' "$CUR_BRANCH"
  printf 'sha=%s\n' "$CUR_SHA"
  printf 'ppid=%s\n' "$PPID_OLD"
  printf 'ts=%s\n' "$NOW"
} > "$ENTRY" 2>/dev/null || true

exit 0
