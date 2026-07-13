#!/usr/bin/env bash
# tail-agent.sh — observe a Claude Code subagent's live progress via its worktree.
#
# Subagents dispatched with `isolation: "worktree"` work in:
#   .claude/worktrees/agent-<agentId>/
#
# Real-time visibility into agent activity comes from watching the worktree:
# new files appear, existing files get modified, commits land. This script wraps
# `git status` + `git log` + `git diff --stat` snapshots in a watch loop.
#
# Usage:
#   bash agents/scripts/core/tail-agent.sh                 # watch the most-recent worktree
#   bash agents/scripts/core/tail-agent.sh <idPrefix>      # watch a specific agent (prefix-match)
#   bash agents/scripts/core/tail-agent.sh --list          # list all worktree-agents (newest first)
#   bash agents/scripts/core/tail-agent.sh --diff [id]     # also show git diff --stat per tick
#   bash agents/scripts/core/tail-agent.sh --once [id]     # print state once + exit (no watch)
#   bash agents/scripts/core/tail-agent.sh --interval N    # tick every N seconds (default 3)
#   bash agents/scripts/core/tail-agent.sh --files [id]    # show files-modified-recently instead
#
# Examples:
#   bash agents/scripts/core/tail-agent.sh -l
#   bash agents/scripts/core/tail-agent.sh ade88dbd
#   bash agents/scripts/core/tail-agent.sh --diff --interval 5

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORKTREES_DIR="$REPO_ROOT/.claude/worktrees"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

list_worktrees() {
    if [ ! -d "$WORKTREES_DIR" ]; then
        echo "no worktrees dir at: $WORKTREES_DIR" >&2
        return 1
    fi
    # Sort by mtime desc; show id + age + last-touched file + size of work in progress.
    find "$WORKTREES_DIR" -maxdepth 1 -mindepth 1 -type d -name 'agent-*' \
        -printf '%T@ %p\n' 2>/dev/null \
        | sort -rn \
        | while read -r mtime path; do
            agent_id="$(basename "$path" | sed 's/^agent-//')"
            human_time="$(date -d "@${mtime%.*}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "$mtime")"
            # Branch name from .git file (worktree refs a parent .git)
            branch=""
            if [ -d "$path" ]; then
                branch="$(cd "$path" 2>/dev/null && git symbolic-ref --short HEAD 2>/dev/null || echo "(detached)")"
            fi
            printf '%s  %s  %s\n' "$human_time" "$agent_id" "${branch}"
        done
}

resolve_worktree() {
    local id="${1:-}"
    if [ -n "$id" ]; then
        # Prefix match — return newest match.
        find "$WORKTREES_DIR" -maxdepth 1 -mindepth 1 -type d -name "agent-${id}*" \
            -printf '%T@ %p\n' 2>/dev/null \
            | sort -rn | head -1 | awk '{$1=""; sub(/^ /, ""); print}' || true  # head can SIGPIPE the upstream find/sort under pipefail
    else
        # No id — most recent.
        find "$WORKTREES_DIR" -maxdepth 1 -mindepth 1 -type d -name 'agent-*' \
            -printf '%T@ %p\n' 2>/dev/null \
            | sort -rn | head -1 | awk '{$1=""; sub(/^ /, ""); print}' || true  # head can SIGPIPE the upstream find/sort under pipefail
    fi
}

snapshot_worktree() {
    local wt="$1"
    local show_diff="${2:-0}"
    local show_files="${3:-0}"

    echo "=== $(date '+%H:%M:%S')  $(basename "$wt") ==="
    if ! cd "$wt" 2>/dev/null; then
        echo "  (worktree gone)"
        return 1
    fi

    # Branch + HEAD position.
    local branch head_sha
    branch="$(git symbolic-ref --short HEAD 2>/dev/null || echo "(detached)")"
    head_sha="$(git rev-parse --short HEAD 2>/dev/null || echo "?")"
    echo "  branch: $branch @ $head_sha"

    # Commits ahead of develop.
    local ahead
    ahead="$(git rev-list --count "$head_sha"..."origin/develop" 2>/dev/null | head -1 || echo "?")"
    local commits_ahead
    commits_ahead="$(git rev-list --count "origin/develop"..."$head_sha" 2>/dev/null || echo "0")"
    echo "  $commits_ahead commits ahead of origin/develop"

    # Working tree status.
    local status_lines
    status_lines="$(git status --porcelain 2>/dev/null | wc -l || echo 0)"
    echo "  $status_lines uncommitted file changes"
    if [ "$status_lines" -gt 0 ]; then
        git status --short 2>/dev/null | sed 's/^/    /' | head -20 || true  # head SIGPIPE under pipefail
    fi

    # Recent commits on this branch (above develop).
    if [ "$commits_ahead" != "0" ] && [ "$commits_ahead" -gt 0 ]; then
        echo "  recent commits:"
        git log --oneline "origin/develop"..HEAD 2>/dev/null | sed 's/^/    /' | head -5 || true  # head SIGPIPE under pipefail
    fi

    if [ "$show_diff" -eq 1 ]; then
        echo "  diff --stat (vs origin/develop):"
        git diff --stat "origin/develop"...HEAD 2>/dev/null | sed 's/^/    /' | tail -15
    fi

    if [ "$show_files" -eq 1 ]; then
        echo "  recently modified (last 5 min):"
        find . -type f -mmin -5 \
            ! -path './.git/*' \
            ! -path './build/*' \
            ! -path './.fetchcontent-src/*' \
            2>/dev/null | sed 's/^/    /' | head -15 || true  # head SIGPIPE under pipefail
    fi
    echo
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

show_diff=0
show_files=0
once=0
interval=3
id=""

while [ $# -gt 0 ]; do
    case "$1" in
        --list|-l)     list_worktrees; exit ;;
        --diff)        show_diff=1; shift ;;
        --files)       show_files=1; shift ;;
        --once)        once=1; shift ;;
        --interval)    interval="${2:-3}"; shift 2 ;;
        --interval=*)  interval="${1#--interval=}"; shift ;;
        -h|--help)     grep '^# ' "$0" | sed 's/^# //'; exit 0 ;;
        --)            shift; id="${1:-}"; shift || true ;;
        -*)            echo "unknown flag: $1" >&2; exit 2 ;;
        *)             id="$1"; shift ;;
    esac
done

wt="$(resolve_worktree "$id")"
if [ -z "$wt" ]; then
    if [ -n "$id" ]; then
        echo "no worktree matching 'agent-${id}*' under $WORKTREES_DIR" >&2
    else
        echo "no agent worktrees under $WORKTREES_DIR" >&2
    fi
    echo "(run with --list to see options)" >&2
    exit 1
fi

# Ensure origin/develop is reachable for diff stats (no fetch — just touch local ref).
( cd "$wt" 2>/dev/null && git rev-parse "origin/develop" >/dev/null 2>&1 ) \
    || echo "# note: origin/develop ref not present in this worktree (diff stats may be empty)" >&2

progress_log="$wt/.progress.log"

# If the agent is emitting progress markers per AGENTS.md § Subagent progress
# markers, `.progress.log` exists at the worktree root. Prefer tailing it —
# it's lower-noise than git-snapshot polling and updates in near-real-time.
if [ -f "$progress_log" ]; then
    echo "# tailing .progress.log: $progress_log" >&2
    echo "# (Ctrl+C to stop; falls through to git snapshot at end)" >&2
    if [ "$once" -eq 1 ]; then
        cat "$progress_log"
        echo
        snapshot_worktree "$wt" "$show_diff" "$show_files"
    else
        tail -n 50 -f "$progress_log"
    fi
    exit 0
fi

if [ "$once" -eq 1 ]; then
    snapshot_worktree "$wt" "$show_diff" "$show_files"
else
    echo "# watching: $wt (every ${interval}s — Ctrl+C to stop)" >&2
    echo "# (no .progress.log present — agent may not emit progress markers; falling back to git snapshot)" >&2
    echo
    while true; do
        snapshot_worktree "$wt" "$show_diff" "$show_files"
        sleep "$interval"
    done
fi
