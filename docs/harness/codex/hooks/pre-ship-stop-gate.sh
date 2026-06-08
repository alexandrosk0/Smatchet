#!/usr/bin/env bash
# Codex Stop hook wrapper around the shared project pre-ship gate.
#
# The existing implementation lives under docs/harness/claude-code/hooks because
# Claude Code was the first runtime with Stop hooks. Keep this wrapper separate
# so Codex setup does not copy or depend on .claude/ runtime files.

set -u

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
export CLAUDE_PROJECT_DIR="$ROOT"

bash "$ROOT/docs/harness/claude-code/hooks/pre-ship-stop-gate.sh"
