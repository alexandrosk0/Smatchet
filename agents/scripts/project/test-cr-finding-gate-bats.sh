#!/usr/bin/env bash
# test-cr-finding-gate-bats.sh — bats wrapper for tests/bats/cr_finding_gate.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob (that glob —
# not a tests/bats/*.bats glob — is what enrols a suite, so an unwrapped bats
# file runs locally and never in CI; test-orphan-bats catches exactly that).
# Mirrors test-about-buildinfo-bats.sh.
#
# The wrapped suite lints the CR finding gate's timeout ordering and its
# fallback status poster — pure text assertions over two YAML files, no network
# and no toolchain, so it runs anywhere bats does.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-cr-finding-gate-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/cr_finding_gate.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-cr-finding-gate-bats: $BATS_FILE not found" >&2
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
# a harness that dies before the plan line) leaves PASSED=FAILED=0, which a
# FAILED-only gate would report as green — the fail-open shape.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "test-cr-finding-gate-bats: no TAP results parsed from $BATS_FILE (expected >= 1)" >&2
    exit 1
fi

if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then
    exit 1
fi
exit 0
