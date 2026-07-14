#!/usr/bin/env bash
# Manual flush of the deferred-lint queue.
#
# The PostToolUse hook (.claude/hooks/lint-cpp.sh) queues edited files and
# defers cppcheck + clang-tidy + dual-target syntax until the Stop event fires
# .claude/hooks/lint-cpp-drain.sh. Run this script to drain the queue
# explicitly — useful when an agent wants lint findings before reporting done
# or when a session is paused without a natural Stop boundary.
#
# Exits 0 on clean drain, 2 on findings (same contract as the Stop hook).

set -euo pipefail

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"

exec bash "$PROJ_DIR/.claude/hooks/lint-cpp-drain.sh"
