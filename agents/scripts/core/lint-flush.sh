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

# The queue and the drain hook live in the project's .claude/ — host content, so
# the default is PROJECT_ROOT (the superproject once the layer is the agent-layer/
# submodule), not this script's own tree.
_lf_self="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${CLAUDE_PROJECT_DIR:-}" ] && [ -f "$_lf_self/../../../scripts/dev/project-config.sh" ]; then
    # shellcheck source=scripts/dev/project-config.sh
    PC_ROOTS_ONLY=1 . "$_lf_self/../../../scripts/dev/project-config.sh" || true
fi
PROJ_DIR="${CLAUDE_PROJECT_DIR:-${PROJECT_ROOT:-$(cd "$_lf_self/../../.." && pwd)}}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"

exec bash "$PROJ_DIR/.claude/hooks/lint-cpp-drain.sh"
