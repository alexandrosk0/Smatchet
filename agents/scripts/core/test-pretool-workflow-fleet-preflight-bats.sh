#!/usr/bin/env bash
# test-pretool-workflow-fleet-preflight-bats.sh — bats wrapper for
# tests/bats/pretool_workflow_fleet_preflight.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Wraps the regression suite for the PreToolUse hook
# docs/harness/claude-code/hooks/pretool-workflow-fleet-preflight.sh — it runs
# fleet-preflight.sh --strict on a Workflow fan-out and BLOCKS (exit 2) a launch
# with a hard violation in an above-threshold (>2-agent) fan-out, while a clean
# fan-out, a 1-2 agent helper, a name-only/empty payload, or a missing-jq host all
# pass (fail-open). Emits the canonical `Passed: N  Failed: M` line test-all.sh
# greps for.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-pretool-workflow-fleet-preflight-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "test-pretool-workflow-fleet-preflight-bats: jq not on PATH (the hook + tests need it)." >&2
    echo "Passed: 0  Failed: 0  (skipped — jq missing)"
    exit 2
fi

BATS_FILE="tests/bats/pretool_workflow_fleet_preflight.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-pretool-workflow-fleet-preflight-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?

echo "$OUT"

passed="$(printf '%s\n' "$OUT" | grep -cE '^ok ')"
failed="$(printf '%s\n' "$OUT" | grep -cE '^not ok ')"
echo "Passed: $passed  Failed: $failed"

[ "$RC" -eq 0 ] && [ "$failed" -eq 0 ] && exit 0
exit 1
