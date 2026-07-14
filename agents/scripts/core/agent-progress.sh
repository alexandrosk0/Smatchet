#!/usr/bin/env bash
# agent-progress.sh — subagent emits a one-line progress marker.
#
# Writes to `<repo-or-worktree-root>/.progress.log` (gitignored). The line goes
# on its own process so each call flushes to disk immediately — `tail -f` on the
# file sees updates within ~1s. Format mirrors `[HH:MM:SS] <phase>: <message>`.
#
# Usage:
#   bash agents/scripts/core/agent-progress.sh "<phase>: <one-line message>"
#   bash agents/scripts/core/agent-progress.sh start "cutting feat/foo branch off develop"
#   bash agents/scripts/core/agent-progress.sh gate "ninja-test-msvc building"
#
# Phase conventions (suggested, not enforced):
#   start | lock | design | code | test | gate | commit | push | pr | end
#
# Companion: agents/scripts/core/tail-agent.sh consumes this file when present.

set -euo pipefail

# Single-string form: "phase: text". Two-arg form: phase text.
if [ "$#" -eq 0 ]; then
    echo "usage: bash agents/scripts/core/agent-progress.sh <phase>: <text>" >&2
    echo "   or: bash agents/scripts/core/agent-progress.sh <phase> <text>" >&2
    exit 2
fi

if [ "$#" -eq 1 ]; then
    message="$1"
else
    phase="$1"
    shift
    message="${phase}: $*"
fi

# Find the worktree root (where .git is, or parent that has it).
root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
log="${root}/.progress.log"

ts="$(date '+%H:%M:%S')"
# Append + sync. `>>` on a per-invocation shell flushes on exit.
printf '[%s] %s\n' "$ts" "$message" >> "$log"
