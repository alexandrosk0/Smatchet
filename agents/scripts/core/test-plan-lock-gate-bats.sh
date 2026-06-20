#!/usr/bin/env bash
# test-plan-lock-gate-bats.sh — bats wrapper for tests/bats/plan_lock_gate.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Wraps the headless suite for Layer C of the plan-lock enforcement
# (agents/scripts/core/plan-lock-gate.sh): cross-branch overlap reds, my-own
# branch / no-lock / stale / unattributable pass, fail-CLOSED on an unresolvable
# base, and the three-dot == merge-base symmetry with Layer B. Emits the
# canonical `Passed: N Failed: M` line test-all.sh greps.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools lists the install)

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-plan-lock-gate-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/plan_lock_gate.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-plan-lock-gate-bats: $BATS_FILE not found" >&2
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
