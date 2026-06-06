#!/usr/bin/env bash
# session-tree-banner.sh — SessionStart hook for concurrent-session safety.
#
# Three jobs:
#   1. Record THIS session's HEAD baseline {branch,sha,ppid,ts} in the per-tree
#      registry (<tree>/.claude/.active-sessions/<session_id>). guard-head-drift.sh
#      reads this to detect HEAD moving under the session.
#   2. Lazily sweep registry entries older than 7d (the only hard cleanup — there
#      is no SessionEnd hook; Stop fires per-turn and only heartbeats the ts).
#   3. Print a banner: integration tree (shared) vs isolated worktree, plus a live
#      sibling count and a nudge toward worktree-per-session.
#
# Banner goes to stdout (Claude Code injects SessionStart stdout as context).
# Never blocks; exit 0 always.
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.

set -u

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"

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

# 2. Sweep dead entries (> 7 days).
if [ -d "$REGDIR" ]; then
  for f in "$REGDIR"/*; do
    [ -f "$f" ] || continue
    fts="$(sed -n 's/^ts=//p' "$f" | head -n1)"
    case "$fts" in ''|*[!0-9]*) continue ;; esac
    [ $((NOW - fts)) -gt 604800 ] && rm -f "$f" 2>/dev/null || true
  done
fi

# 1. Record / refresh own baseline.
if [ -n "$SESSION_ID" ] && [ -n "$CUR_BRANCH" ] && [ -n "$CUR_SHA" ]; then
  {
    printf 'branch=%s\n' "$CUR_BRANCH"
    printf 'sha=%s\n' "$CUR_SHA"
    printf 'ppid=%s\n' "$PPID"
    printf 'ts=%s\n' "$NOW"
  } > "$REGDIR/$SESSION_ID" 2>/dev/null || true
fi

# 3. Banner.
if [ -d "$PROJ/.git" ]; then
  # Integration (main) tree — count live siblings (exclude self).
  live=0
  if [ -d "$REGDIR" ]; then
    for f in "$REGDIR"/*; do
      [ -f "$f" ] || continue
      [ "$(basename "$f")" = "$SESSION_ID" ] && continue
      fts="$(sed -n 's/^ts=//p' "$f" | head -n1)"
      fpid="$(sed -n 's/^ppid=//p' "$f" | head -n1)"
      fresh=0
      case "$fts" in ''|*[!0-9]*) : ;; *) [ $((NOW - fts)) -lt 1800 ] && fresh=1 ;; esac
      alive=0
      case "$fpid" in ''|*[!0-9]*) : ;; *) kill -0 "$fpid" 2>/dev/null && alive=1 ;; esac
      { [ "$fresh" = 1 ] || [ "$alive" = 1 ]; } && live=$((live + 1))
    done
  fi
  printf '🌳 Integration tree (%s) on branch %s. ' "$PROJ" "${CUR_BRANCH:-?}"
  [ "$live" -gt 0 ] && printf '⚠ %s other live session(s) share this tree — HEAD/branch changes WILL collide. ' "$live"
  printf 'Prefer one worktree per session: `nsc <slug>` (or pwsh scripts/dev/worktree.ps1 new <slug>). Direct commits to develop/main here are blocked.\n'
else
  printf '🌿 Isolated worktree (%s) on branch %s — HEAD is private to this session.\n' "$PROJ" "${CUR_BRANCH:-?}"
fi

exit 0
