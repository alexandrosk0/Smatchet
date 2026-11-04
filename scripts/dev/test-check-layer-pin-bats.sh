#!/usr/bin/env bash
# test-check-layer-pin-bats.sh — bats wrapper for tests/bats/check_layer_pin.bats.
#
# Bucket A (CLI). Zero manual steps. Auto-enrolled by scripts/dev/test-all.sh via
# the test-*.sh glob. Emits the canonical `Passed: N  Failed: M` line test-all.sh
# greps for. Mirrors test-about-buildinfo-bats.sh.
#
# The wrapped suite covers the agent-layer pin-on-develop gate
# (scripts/dev/check-layer-pin.sh) against throwaway git repos; it needs only
# bash, git and bats.
#
# Fail-closed: a run with no parsed TAP result fails (the PASSED floor below).
#
# Exit codes follow the test-script convention (scripts/dev/test-all.sh header):
#   0 — every bats test passed
#   1 — at least one bats test failed, or no TAP result was parsed
#   2 — bats binary missing

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-check-layer-pin-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           brew install bats-core  (macOS)
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/check_layer_pin.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-check-layer-pin-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?

echo "$OUT"

PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)

echo "Passed: ${PASSED}  Failed: ${FAILED}"

# PASSED floor. A bats run that emits no TAP at all (vanished file, parse error,
# a suite with zero @test cases, a harness that dies before the plan line) leaves
# PASSED=FAILED=0, which a FAILED-only gate would report as green — the fail-open
# shape.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "test-check-layer-pin-bats: no TAP results parsed from $BATS_FILE (expected >= 1)" >&2
    exit 1
fi

if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then
    exit 1
fi
exit 0
