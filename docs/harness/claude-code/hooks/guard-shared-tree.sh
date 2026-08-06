#!/usr/bin/env bash
# guard-shared-tree.sh — PreToolUse(Bash) aggressor guard (SECONDARY).
#
# In the shared integration tree only, block a HEAD/working-tree-mutating git op
# (checkout / switch / pull / reset / merge / rebase / stash pop) when ANOTHER
# session is live in that tree — such an op would rug-pull the sibling.
#
# This is advisory defense-in-depth (registry-dependent, bias-to-allow on
# uncertainty). The hard net is guard-head-drift.sh, which protects the victim
# regardless of who moved HEAD. Recovery from a drift is `worktree.sh resync` or
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

# Drop heredoc BODIES from a command string so a git verb that only appears as
# heredoc text (e.g. `cat <<EOF … git reset … EOF`) can't arm the mutating-op
# match. grep is line-based, so a body line `git reset` otherwise matches `^git`.
# The redirection line itself is kept (it carries no executable verb). Best-effort
# (single nesting level, word delimiter); over-dropping only makes this advisory
# guard MORE permissive, never falsely DENY. (tooling.md 2026-06-18 :566)
strip_heredoc() {
  local line state=0 delim="" out=""
  local hdre='<<-?[[:space:]]*["'"'"']?([A-Za-z_][A-Za-z0-9_]*)'
  while IFS= read -r line || [ -n "$line" ]; do
    if [ "$state" = 1 ]; then
      [[ "$line" =~ ^[[:space:]]*"$delim"[[:space:]]*$ ]] && state=0
      continue
    fi
    [[ "$line" =~ $hdre ]] && { delim="${BASH_REMATCH[1]}"; state=1; }
    out+="$line"$'\n'
  done <<< "$1"
  printf '%s' "$out"
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

# Mutating git ops only. Match `git` ONLY at a command position — start of a
# line (grep is line-based, so this also covers a `;`/`&&`/newline-separated op)
# or right after a `;` `&` `|` `(` `&&` `||` separator — never after a bare word
# (an argument) or a quote. This is what stops a verb that merely appears as an
# echo/commit-message argument or a quoted string from arming the guard; heredoc
# bodies are removed first by strip_heredoc. (tooling.md 2026-06-18 :566)
STRIPPED="$(strip_heredoc "$CMD")"
cmdpos='(^|[;&|(]|&&|\|\|)[[:space:]]*'
gitopts='([[:space:]]+(-C[[:space:]]+("[^"]*"|[^[:space:]]+)|-c[[:space:]]+[^[:space:]]+|--[^[:space:]]+))*'
printf '%s' "$STRIPPED" | grep -qE "${cmdpos}git${gitopts}[[:space:]]+(checkout|switch|pull|reset|merge|rebase)(\$|[[:space:]])|${cmdpos}git[[:space:]]+stash[[:space:]]+pop(\$|[[:space:]])" || exit 0

# Worktree-target exemption: an op that targets a DIFFERENT git worktree than
# this integration tree moves THAT worktree's HEAD, never the shared tree's — so
# it can't rug-pull a sibling here. EXEMPT only when EVERY mutating-op invocation
# carries its own explicit `-C <worktree>`. We do NOT try to model `cd` — shell
# cwd tracking across `&&`/`||`/`;`/subshells/`cd`-back is not reliably derivable
# from the raw command string, so a `cd`-based exemption is unsound (a later op
# can re-target the shared tree, e.g. `cd <wt> && git -C <integration> merge`);
# the canonical cross-worktree form is `git -C <ABSOLUTE-worktree-path> <op>`,
# and `-C` is verified PER-OP so a `-C` on a LATER op can't exempt an earlier
# bare op that runs in the shared tree (Cursor #1388).
# This guard is ADVISORY / bias-to-allow (the hard net is guard-head-drift.sh):
#   - target resolves to a different worktree  -> safe
#   - target is an UNEXPANDED $VAR (can't stat) -> safe (almost always a
#     worktree; a `-C $VAR` is never the shared tree literally — :10)
#   - target resolves to THIS integration tree, or a bare op -> NOT safe (block)
_proj_top="$(git -C "$PROJ" rev-parse --show-toplevel 2>/dev/null)"
wt_exempt() { # $1 = candidate path (raw, possibly quoted / $VAR)
  local p="$1"
  [ -n "$p" ] || return 1
  case "$p" in *'$'*) return 0 ;; esac          # unexpanded var -> advisory allow
  local top
  top="$(git -C "$p" rev-parse --show-toplevel 2>/dev/null)"
  [ -n "$top" ] && [ -n "$_proj_top" ] && [ "$top" != "$_proj_top" ]
}

# Enumerate ONLY command-position mutating-op invocations (same ${cmdpos} prefix
# as the detection grep, so a verb that merely appears as an argument — e.g.
# `… && echo git reset` — is NOT picked up here and can't force a false block).
# Each real invocation must carry its own worktree `-C`; one bare / integration-
# targeted op keeps the block.
opmatch="${cmdpos}git${gitopts}[[:space:]]+((checkout|switch|pull|reset|merge|rebase)|stash[[:space:]]+pop)"
all_safe=1
while IFS= read -r _m; do
  [ -n "$_m" ] || continue
  _tgt="$(printf '%s' "$_m" | grep -oE '\-C[[:space:]]+("[^"]*"|[^[:space:]]+)' | tail -n1 | sed -E 's/^-C[[:space:]]+//; s/^"//; s/"$//')"
  wt_exempt "$_tgt" || { all_safe=0; break; }
done <<OPMATCHES
$(printf '%s' "$STRIPPED" | grep -oE "$opmatch")
OPMATCHES
[ "$all_safe" = 1 ] && exit 0

SID="$(json_field '.session_id' 'session_id')"
[ -n "$SID" ] || SID="${CLAUDE_SESSION_ID:-}"

REGDIR="$PROJ/.claude/.active-sessions"
[ -d "$REGDIR" ] || exit 0

NOW="$(date -u +%s)"
live="$(sr_count_live_siblings "$REGDIR" "$SID" "$NOW")"

if [ "$live" -gt 0 ]; then
  reason="${live} concurrent session(s) share this integration tree (${PROJ}); this op would change HEAD/working-tree under them. Do feature work in a worktree: bash scripts/dev/worktree.sh new <slug>. To act on a worktree FROM here, target it explicitly — \`git -C <ABSOLUTE-worktree-path> <op>\` (a LITERAL path, not a \$VAR — this guard reads the un-expanded command text; a bare \`cd <wt> && git …\` is NOT exempt, the op must carry its own -C). Override: export SMATCHET_ALLOW_SHARED_SWITCH=1 in the session env BEFORE launch (an inline \`SMATCHET_ALLOW_SHARED_SWITCH=1 git …\` prefix does NOT work — the hook reads its own env before your command runs)."
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$reason")"
fi
exit 0
