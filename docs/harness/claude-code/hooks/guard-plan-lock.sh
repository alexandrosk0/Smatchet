#!/usr/bin/env bash
# guard-plan-lock.sh — PreToolUse(Edit|Write|MultiEdit|NotebookEdit) plan-lock
# guard (Layer A of the 3-layer force-on-contention enforcement).
#
# Force lock-filing ONLY when it matters: a write set must be claimed before
# edits IFF another session is concurrently live (force-on-contention). Solo /
# sequential work — the common case — is never blocked; it pays at most one
# cross-edit-cached liveness probe, never a per-edit tax on BLOCKING.
#
# When >=1 other session is live AND the edited path is NOT covered by a
# refs/locks/* claim whose `branch` == this worktree's HEAD branch (this
# session's own seed), DENY with a copy-paste lock-claim.sh remediation. Solo
# (0 siblings) -> allow. Identity is keyed on BRANCH, never owner/AGENT_ID
# (unprovisioned in prod). Same-tree-same-branch contention is out of scope (the
# tree guards own that); plan-locks target cross-BRANCH parallel work.
#
# ADVISORY / FAIL-OPEN: any missing lib / network / parse / worktree-enum error,
# a detached HEAD, an unresolvable path, or a blown hook timeout -> allow. The
# authoritative nets are Layer B (every push) and Layer C (the CI gate). This
# hook runs alongside guard-head-drift.sh on the edit matcher; both ALWAYS run
# and a deny from either wins (array order is cosmetic) — the common-case
# cheapness comes only from this hook's own solo `exit 0`, never from a sibling
# hook short-circuiting.
#
# Allow = exit 0 (no stdout). Deny = the PreToolUse permissionDecision JSON.
# Override: export SMATCHET_REQUIRE_PLAN_LOCK=0 in the session env BEFORE launch.
#
# See docs/plans/plan-lock-enforcement.md (Layer A) +
#     docs/agent-rules/process-rules.md § Concurrent interactive sessions.

set -u

# Override reaches the hook only via the session env (the hook reads its own env
# before the tool runs; an inline `SMATCHET_REQUIRE_PLAN_LOCK=0 <edit>` prefix
# does NOT work).
[ "${SMATCHET_REQUIRE_PLAN_LOCK:-1}" = "0" ] && exit 0

json_escape() { # backslash + double-quote only — reasons are single-line.
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

deny() {
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$1")"
  exit 0
}

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

FP="$(json_field '.tool_input.file_path' 'file_path')"
[ -n "$FP" ] || exit 0   # no edit target -> nothing to gate

# own session id: stdin-preferred, env fallback — the SAME effective precedence
# session-tree-banner.sh writes the registry basename with, so self-exclusion
# can't miss its own entry and gate solo work (the F2 fix). NEVER sr_session_pid
# (a ~300-500ms powershell ancestor walk this hot path must not pay).
SID="$(json_field '.session_id' 'session_id')"
[ -n "$SID" ] || SID="${CLAUDE_SESSION_ID:-}"

# Shared substrate lib. The hook is COPIED into .claude/hooks/ at setup, so it
# can't source a sibling — resolve via CLAUDE_PROJECT_DIR (runtime), then the git
# top-level of the hook's own location (the in-repo source path the bats run).
# Missing lib -> fail OPEN.
_ltc_lib="$PROJ/agents/scripts/core/lock-table-cache.sh"
if [ ! -f "$_ltc_lib" ]; then
  _top="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null)"
  [ -n "$_top" ] && _ltc_lib="$_top/agents/scripts/core/lock-table-cache.sh"
fi
[ -f "$_ltc_lib" ] || exit 0
export LTC_PROJ="$PROJ"
# shellcheck source=agents/scripts/core/lock-table-cache.sh
. "$_ltc_lib"

# Force-on-contention: 0 live siblings -> never block (no tax on solo BLOCKING).
# Count is repo-global + cross-edit-cached; fail-open to 0 on any error.
count="$(ltc_sibling_count "$SID")"
case "$count" in ''|*[!0-9]*) exit 0 ;; esac
# >=1 sibling == >=2 agents total == contended (own is already excluded; the
# off-by-one trap is that "excluding self" pairs with a threshold of >=1, not 2).
[ "$count" -ge 1 ] || exit 0

# Discriminator is this worktree's HEAD branch (my own seed lock). Detached /
# unresolvable -> fail-open allow (an unattributable session can't be keyed).
mybr="$(git -C "$PROJ" symbolic-ref --short HEAD 2>/dev/null)" || exit 0
[ -n "$mybr" ] || exit 0

# Resolve the edited path to repo-relative; an edit OUTSIDE this repo is not ours
# to gate. Normalize separators so a Windows backslash path strips cleanly.
PROJ_TOP="$(git -C "$PROJ" rev-parse --show-toplevel 2>/dev/null)" || exit 0
[ -n "$PROJ_TOP" ] || exit 0
FP_N="${FP//\\//}"; TOP_N="${PROJ_TOP//\\//}"
case "$FP_N" in
  "$TOP_N"/*) rel="${FP_N#"$TOP_N"/}" ;;
  *) exit 0 ;;
esac
[ -n "$rel" ] || exit 0

# Allow iff covered by a lock whose branch == mine. rc: 0 covered, 1 not, 2
# undetermined (table unavailable -> fail-open allow).
slug="$(ltc_covering_slug "$rel" same "$mybr")"; cov=$?
case "$cov" in
  0|2) exit 0 ;;
esac

deny "Plan-lock: ${count} other live session(s) share this repo and you hold no plan-lock covering '${rel}' on branch '${mybr}'. Claim one before editing — write the paths you will touch (one per line, repo-relative) to a file, then run: LOCK_PLAN=<your plan-doc> bash agents/scripts/core/lock-claim.sh <slug> <that-file>. Already hold a lock for this work but missed a file? Grow it: bash agents/scripts/core/lock-claim-update.sh <slug> <full-write-set-file> (it REPLACES the set, so list every path). Override: export SMATCHET_REQUIRE_PLAN_LOCK=0 in the session env BEFORE launch (an inline prefix does NOT reach this hook)."
