#!/usr/bin/env bash
# check-main-repo-clean.sh — surface wrong-worktree-path edits at session end.
#
# Problem (process.md 2026-05-19 orchestrator P2 — second documented hit):
# orchestrator running inside a `.claude/worktrees/<id>/` worktree sometimes
# writes Edit/Write absolute paths against the main repo (`C:\Dev\Smatchet\...`)
# instead of the worktree-rooted path. Edits land on whatever branch the main
# repo is currently checked out on (often a parallel agent's branch); the
# orchestrator's actual worktree gets none of the work. Documentation alone
# hasn't prevented recurrence.
#
# This script runs on Stop hook (per .claude/settings.json) and:
#   1. Resolves the main repo via `git rev-parse --git-common-dir`.
#   2. Runs `git -C <main> status --short`.
#   3. If non-empty, prints a loud STDERR warning naming each modified file
#      so the orchestrator (or the user reading the transcript) sees the
#      stale state before the session ends.
#   4. Returns 0 always — never blocks the Stop event. Best-effort signal.
#
# Manual invocation (anytime, not just on Stop):
#   bash agents/scripts/core/check-main-repo-clean.sh
#
# Authoritative spec: docs/self-improvement/categories/process.md
# (2026-05-19 orchestrator P2 — option b).

set -euo pipefail

# Resolve main repo via the worktree's common dir (`.git` file in worktrees
# points at the canonical `.git/worktrees/<id>` dir inside the main repo).
common_dir=$(git rev-parse --git-common-dir 2>/dev/null || true)
if [[ -z "$common_dir" ]]; then
  exit 0
fi

# git-common-dir returns the path to the `.git` directory of the main repo.
# The repo root is its parent (when the path is `<repo>/.git`) or three
# parents up when we're inside a worktree (`<repo>/.git/worktrees/<id>/`).
case "$common_dir" in
  */\.git/worktrees/*)
    # Worktree case: <repo>/.git/worktrees/<id>/ → main repo at <repo>/.
    main_repo=$(cd "$common_dir/../.." && pwd)
    ;;
  */\.git)
    # Already in main repo: <repo>/.git → main repo at <repo>/.
    main_repo=$(cd "$common_dir/.." && pwd)
    ;;
  *)
    # Unknown shape; skip to avoid false alarm.
    exit 0
    ;;
esac

# Are we actually in a worktree (vs the main repo)? If main repo IS this
# worktree, nothing to check (session-end audit only matters when there's a
# separate main repo that could have absorbed wrong-worktree-path edits).
current_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z "$current_root" || "$current_root" = "$main_repo" ]]; then
  exit 0
fi

# Inventory main repo state.
status_out=$(git -C "$main_repo" status --short 2>/dev/null || true)
if [[ -z "$status_out" ]]; then
  exit 0
fi

# Non-empty → wrong-worktree-path footgun likely. Loud warning to STDERR.
main_branch=$(git -C "$main_repo" branch --show-current 2>/dev/null || echo "(detached HEAD)")
worktree_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "(detached HEAD)")

{
  echo ""
  echo "=========================================================================="
  echo " WARNING: main repo has uncommitted modifications at session end"
  echo "=========================================================================="
  echo " Main repo:        $main_repo"
  echo " Main branch:      $main_branch"
  echo " This worktree:    $current_root"
  echo " Worktree branch:  $worktree_branch"
  echo ""
  echo " Modified files in main repo:"
  echo "$status_out" | sed 's/^/   /'
  echo ""
  echo " Likely cause: an Edit/Write call this session used an absolute path"
  echo " missing the worktree prefix \"$current_root\". Edits landed in the"
  echo " main repo instead of the worktree. Recover via:"
  echo ""
  echo "   git -C \"$main_repo\" stash push -m \"recovery: wrong-worktree-path edits\""
  echo "   # then replay the edits against the worktree paths."
  echo ""
  echo " Reference: docs/self-improvement/categories/process.md"
  echo "            (2026-05-19 orchestrator P2)"
  echo "=========================================================================="
} >&2

# Best-effort signal; never block the Stop event.
exit 0
