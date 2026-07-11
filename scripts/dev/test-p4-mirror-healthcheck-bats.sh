#!/usr/bin/env bash
# test-p4-mirror-healthcheck-bats.sh — bats wrapper for tests/bats/p4_mirror_healthcheck.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. Added by
# pr-intent-capture-hardening #3 (orphan-bats gate): this suite previously had no
# wrapper, so it never ran. See that bats file's header for the system under test.
#
# Exit codes:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    echo "test-p4-mirror-healthcheck-bats: bats not on PATH (npm i -g bats). See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/p4_mirror_healthcheck.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-p4-mirror-healthcheck-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?
echo "$OUT"
PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${PASSED}  Failed: ${FAILED}"
# Zero-run floor (fail-open shape Z): a bats suite that parses to ZERO tests
# (vanished file / TAP parse error) leaves PASSED=FAILED=0 and would exit green.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the bats suite ran ZERO tests (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then exit 1; fi
exit 0
