#!/usr/bin/env bash
# scripts/dev/new-session.sh — launch a Claude Code session in an ISOLATED worktree.
#
# Bash port of the retired new-session.ps1.
#
# Makes worktree-per-session the default action instead of a discipline you have
# to remember. In the shared integration tree it spins up a fresh worktree (via
# `worktree.sh new <slug>`) and launches claude there; inside a worktree it just
# launches claude in place.
#
# Recommended PowerShell-profile alias (add to $PROFILE):
#   function nsc { param([string]$slug) & bash "C:/Dev/Smatchet/scripts/dev/new-session.sh" $slug }
# Recommended bash alias (~/.bashrc):
#   nsc() { bash /c/Dev/Smatchet/scripts/dev/new-session.sh "$@"; }
#
# Then:  nsc vsync-toggle
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.
#
# Examples:
#   bash scripts/dev/new-session.sh vsync-toggle
#   bash scripts/dev/new-session.sh            # already in a worktree -> just launch

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

die() { echo "new-session: $*" >&2; exit 1; }

case "${1:-}" in
    -h|--help)
        sed -n '2,25p' "${BASH_SOURCE[0]:-$0}"
        exit 0
        ;;
esac

SLUG="${1:-}"

command -v claude >/dev/null 2>&1 || die "claude CLI not found on PATH."

# Detect the tree the CALLER is in (current directory), NOT where this script
# lives — the nsc alias points at one fixed script copy, so script-location would
# always look like the integration tree. The main worktree's toplevel has a .git
# DIRECTORY; a linked worktree's .git is a FILE (gitdir pointer).
TREE="$(git rev-parse --show-toplevel 2>/dev/null | tr -d '\r')"
[ -n "$TREE" ] || die "Not inside a git worktree ($PWD). Run new-session.sh from the integration tree or a worktree."

if [ -d "$TREE/.git" ]; then
    [ -n "$SLUG" ] \
        || die "You are in the integration tree ($TREE). Pass a slug to spin an isolated worktree: new-session.sh <slug>"

    # worktree.sh validates the slug and exits non-zero on failure, but a partial
    # failure could still leave nothing behind — verify the worktree actually
    # materialised before launching.
    bash "$SCRIPT_DIR/worktree.sh" new "$SLUG" || die "worktree.sh new $SLUG failed."

    TREES_ROOT="${SMATCHET_TREES_ROOT:-C:/Dev/trees}"
    TARGET="$TREES_ROOT/$SLUG"
    [ -d "$TARGET" ] || die "Expected worktree at $TARGET was not created — aborting launch."

    echo "Launching claude in $TARGET ..."
    cd "$TARGET" || die "cannot cd to $TARGET"
    exec claude
else
    echo "Already in an isolated worktree ($TREE) — launching claude here."
    exec claude
fi
