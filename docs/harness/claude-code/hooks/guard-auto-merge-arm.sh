#!/usr/bin/env bash
# guard-auto-merge-arm.sh — PreToolUse guard: deny arming GitHub-NATIVE auto-merge
# outside agents/scripts/core/safe-merge.sh.
#
# Native auto-merge waits only on the branch-protection REQUIRED contexts, so it
# merges straight past a red or still-pending NON-required check — the class
# behind postmortems.md 2026-10-04 (#2286, armed through a harness PR tool) and
# the earlier bare `gh pr merge --auto` incidents. safe-merge.sh is the sanctioned
# arm path: it runs the full merge-gates poll and arms `--auto` only on
# GATES_PASSED. Every other arm path creates the same server-side auto-merge
# request, so this hook denies the ones a Claude Code session can reach:
#   * a GitHub-MCP auto-merge tool  (`mcp__<server>__enable_pr_auto_merge`);
#   * a harness PR tool `set_auto_merge` (bare or `mcp__<server>__set_auto_merge`)
#     unless its input explicitly DISABLES (enabled / enable / auto_merge /
#     autoMerge == false);
#   * a Bash / PowerShell command that runs `gh pr merge … --auto` at a command
#     position. `safe-merge.sh <pr>` itself never matches (its own `--auto` runs
#     inside the script, not in the tool command), and disarming
#     (`--disable-auto`, `disable_pr_auto_merge`) is never blocked.
#
# DEFENCE IN DEPTH ONLY: it covers one harness and the command text it is shown
# (a `bash -c '…'` string, a REST/GraphQL call or the web UI are out of its
# reach). The arm-path-independent backstop is the `All checks green
# (block-on-any-red)` aggregate check (agents/scripts/core/all-checks-green.sh)
# once it is a required context. See docs/agent-rules/merge-gates.md
# § Sanctioned non-admin merge path.
#
# Wiring: a PreToolUse matcher in settings.json.tmpl runs the deployed copy at
# .claude/hooks/ (the SessionStart hook sync in clear-session-context.sh copies
# every hook here; sync-settings-hooks.sh heals the matcher into an existing
# settings.json). No env override by design — the sanctioned path exists; a
# human who really wants native auto-merge arms it outside the agent.
#
# Protocol: tool-call JSON on stdin. Allow = exit 0 with no output. Deny = exit 0
# with a permissionDecision JSON. Unparseable input -> allow (fail-open guard).

set -u

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

deny() {
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' \
    "$(json_escape "$1 GitHub-native auto-merge waits only on branch-protection REQUIRED contexts, so it merges past a red or still-pending non-required check. Arm through the sanctioned wrapper instead: bash agents/scripts/core/safe-merge.sh <pr> (runs the full merge-gates poll, arms --auto only on GATES_PASSED). Disarming stays allowed. See docs/agent-rules/merge-gates.md § Sanctioned non-admin merge path.")"
  exit 0
}

INPUT="$(cat || true)"
[ -n "$INPUT" ] || exit 0

HAVE_JQ=0
command -v jq >/dev/null 2>&1 && HAVE_JQ=1

json_field() { # $1 = jq filter, $2 = key for the no-jq sed fallback
  if [ "$HAVE_JQ" = 1 ]; then
    printf '%s' "$INPUT" | jq -r "$1 // empty" 2>/dev/null
  else
    printf '%s' "$INPUT" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n1
  fi
}

TOOL="$(json_field '.tool_name' 'tool_name')"

case "$TOOL" in
  mcp__*__enable_pr_auto_merge)
    deny "Blocked: $TOOL arms GitHub-native auto-merge."
    ;;
  set_auto_merge|mcp__*__set_auto_merge)
    if [ "$HAVE_JQ" = 1 ] && printf '%s' "$INPUT" \
         | jq -e '[.tool_input.enabled, .tool_input.enable, .tool_input.auto_merge, .tool_input.autoMerge] | any(. == false)' \
           >/dev/null 2>&1; then
      exit 0   # an explicit disable — disarming is always allowed
    fi
    deny "Blocked: $TOOL (enable) arms GitHub-native auto-merge."
    ;;
  Bash|PowerShell) ;;
  *) exit 0 ;;
esac

CMD="$(json_field '.tool_input.command' 'command')"
[ -n "$CMD" ] || exit 0

# Join `\`-newline continuations so a wrapped `gh pr merge … \ --auto` is one line.
bsnl=$'\\\n'
CMD="${CMD//"$bsnl"/ }"

# Drop heredoc BODIES: a `gh pr merge … --auto` line inside `cat <<EOF … EOF`
# (a doc or script being written) is text, not a command. Best-effort, one level.
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

# `gh pr merge … --auto` at a COMMAND position only: line start or after a
# ; & | ( ` && || separator, optionally behind VAR=value assignments and a
# command/env/exec/time/nohup/sudo wrapper, `gh` possibly path-qualified. An
# argument that merely mentions it (a commit message, an echo, a grep pattern)
# sits after a quote or a word and never matches. `--auto` must be its own token,
# so `--disable-auto` never matches.
cmdpos='(^|[;&|(`]|&&|\|\|)[[:space:]]*'
assigns='([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+)*'
wrappers='((command|env|exec|time|nohup|sudo)[[:space:]]+)*'
ghbin='([^[:space:];&|()]*/)?gh(\.exe)?'
pattern="${cmdpos}${assigns}${wrappers}${ghbin}[[:space:]]+pr[[:space:]]+merge([[:space:]]+[^;&|[:space:]]+)*[[:space:]]+--auto(=[^[:space:];&|]*)?([[:space:];&|)]|\$)"

if printf '%s\n' "$(strip_heredoc "$CMD")" | grep -qE -- "$pattern"; then
  deny "Blocked: a bare \`gh pr merge … --auto\` arms GitHub-native auto-merge."
fi
exit 0
