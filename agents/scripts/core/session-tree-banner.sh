#!/usr/bin/env bash
# session-tree-banner.sh — SessionStart hook for concurrent-session safety.
#
# Three jobs:
#   1. Record THIS session's HEAD baseline {branch,sha,ppid,ts} in the per-tree
#      registry (<tree>/.claude/.active-sessions/<session_id>). guard-head-drift.sh
#      reads this to detect HEAD moving under the session. ppid is the AUTHORITATIVE
#      session pid (the first claude.exe ancestor on Windows, $PPID on POSIX) — see
#      session-registry-lib.sh for why the old $PPID=1 made liveness useless.
#   2. Cleanup: prune entries that are BOTH provably dead AND ts-stale (the precise
#      net), plus a lazy >7d sweep for legacy/unprovable entries. There is no
#      SessionEnd hook, so SessionStart is the only safe cleanup point (Stop fires
#      per-turn and only heartbeats the ts).
#   3. Print a banner: integration tree (shared) vs isolated worktree, plus a live
#      sibling count and a nudge toward worktree-per-session.
#
# Banner goes to stdout (Claude Code injects SessionStart stdout as context).
# Never blocks; exit 0 always.
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.

set -euo pipefail

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"

# Shared liveness primitives (real-pid liveness + dead+stale prune). Source the
# sibling lib; if it is somehow absent, fall back to degraded inline shims so the
# banner never breaks SessionStart.
_sr_lib="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)/session-registry-lib.sh"  # cd can fail → keep the banner non-blocking; line 30 falls back to $PROJ
[ -f "$_sr_lib" ] || _sr_lib="$PROJ/agents/scripts/core/session-registry-lib.sh"
if [ -f "$_sr_lib" ]; then
  # shellcheck source=agents/scripts/core/session-registry-lib.sh
  . "$_sr_lib"
else
  # Degraded fallback: session-registry-lib.sh is missing, so real-pid liveness is
  # unavailable and sr_session_pid collapses to $PPID (which is 1 under the
  # SessionStart hook's detached parent — the exact case that made liveness
  # useless, see the header). Surface it (hooks-session-lifecycle-04) instead of
  # silently degrading; the banner still runs (non-blocking).
  echo "session-tree-banner: WARN — session-registry-lib.sh not found at '$_sr_lib'; falling back to \$PPID liveness (degraded — sibling-session counts may be wrong). Re-run setup-harness.sh to restore the lib." >&2
  sr_session_pid() { printf '%s' "$PPID"; }
  sr_prune_dead_stale() { :; }
  sr_count_live_siblings() { printf '0'; }
fi

# session id from stdin JSON, falling back to env.
SESSION_ID="${CLAUDE_SESSION_ID:-}"
if [ ! -t 0 ]; then
  RAW="$(cat || true)"
  if [ -n "$RAW" ]; then
    EXTRACTED="$(printf '%s' "$RAW" \
      | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
    [ -n "$EXTRACTED" ] && SESSION_ID="$EXTRACTED"
  fi
fi

REGDIR="$PROJ/.claude/.active-sessions"
mkdir -p "$REGDIR" 2>/dev/null || true

CUR_BRANCH="$(git -C "$PROJ" symbolic-ref --short HEAD 2>/dev/null || true)"
CUR_SHA="$(git -C "$PROJ" rev-parse HEAD 2>/dev/null || true)"
NOW="$(date -u +%s)"

# 2. Cleanup. Precise net first: prune entries that are BOTH provably dead AND
#    ts-stale (never the current session, never a live or unprovable one). Then a
#    lazy >7d sweep mops up legacy/unprovable (ppid=1) entries that the dead-check
#    can't reach.
sr_prune_dead_stale "$REGDIR" "$SESSION_ID" "$NOW"
if [ -d "$REGDIR" ]; then
  for f in "$REGDIR"/*; do
    [ -f "$f" ] || continue
    [ "$(basename "$f")" = "$SESSION_ID" ] && continue
    fts="$(sed -n 's/^ts=//p' "$f" | head -n1)"
    case "$fts" in ''|*[!0-9]*) continue ;; esac
    [ $((NOW - fts)) -gt 604800 ] && rm -f "$f" 2>/dev/null || true
  done
fi

# 1. Record / refresh own baseline (ppid = authoritative session pid, not $PPID).
if [ -n "$SESSION_ID" ] && [ -n "$CUR_BRANCH" ] && [ -n "$CUR_SHA" ]; then
  {
    printf 'branch=%s\n' "$CUR_BRANCH"
    printf 'sha=%s\n' "$CUR_SHA"
    printf 'ppid=%s\n' "$(sr_session_pid)"
    printf 'ts=%s\n' "$NOW"
  } > "$REGDIR/$SESSION_ID" 2>/dev/null || true
fi

# 3. Banner.
if [ -d "$PROJ/.git" ]; then
  # Integration (main) tree — count live siblings (exclude self) via the shared
  # authoritative-pid-preferred rule.
  live="$(sr_count_live_siblings "$REGDIR" "$SESSION_ID" "$NOW")"
  printf '🌳 Integration tree (%s) on branch %s. ' "$PROJ" "${CUR_BRANCH:-?}"
  [ "$live" -gt 0 ] && printf '⚠ %s other live session(s) share this tree — HEAD/branch changes WILL collide. ' "$live"
  printf 'Prefer one worktree per session: `nsc <slug>` (or bash scripts/dev/worktree.sh new <slug>). Direct commits to develop/main here are blocked.\n'
else
  printf '🌿 Isolated worktree (%s) on branch %s — HEAD is private to this session.\n' "$PROJ" "${CUR_BRANCH:-?}"
fi

exit 0
