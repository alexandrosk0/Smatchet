#!/usr/bin/env bash
# test-verifier-endpoint-bats.sh — bats wrapper for tests/bats/verifier_endpoint.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob (and required
# by the orphan-bats gate: a .bats with no wrapper never runs). Covers the
# OpenAI-compatible verifier stub endpoint (drives the loop over real HTTP). See that bats file's header for the
# system under test.
#
# Exit codes:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    echo "test-verifier-endpoint-bats: bats not on PATH (npm i -g bats). See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/verifier_endpoint.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-verifier-endpoint-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?
echo "$OUT"
OK=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
SKIPPED=$(printf '%s\n' "$OUT" | grep -ciE '^ok [0-9]+.*# skip' || true)
PASSED=$((OK - SKIPPED))                       # genuinely-run passes, skips excluded
echo "Passed: ${PASSED}  Failed: ${FAILED}  Skipped: ${SKIPPED}"
# Zero-run floor (fail-open shape Z): a bats suite that parses to ZERO tests
# (vanished file / TAP parse error) leaves OK=FAILED=0 and would exit green.
if [ "$OK" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the bats suite ran ZERO tests (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
# All-skipped floor: a suite where every test skipped (e.g. no python) ran no real
# assertion — treat it as a missing prerequisite (exit 2), never a green pass.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ] && [ "$SKIPPED" -gt 0 ]; then
    echo "$(basename "$0" .sh): SKIPPED - every test skipped (missing prerequisite)." >&2
    exit 2
fi
if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then exit 1; fi
exit 0
