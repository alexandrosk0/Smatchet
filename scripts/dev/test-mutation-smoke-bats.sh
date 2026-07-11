#!/usr/bin/env bash
# test-mutation-smoke-bats.sh — bats wrapper for tests/bats/mutation_smoke.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. The suite drives
# scripts/dev/mutation-smoke.sh classify/floor/honesty paths against a fake rig +
# PATH cmake shim, so it needs no TSan build. Added to close the orphan-bats gap:
# the bats file shipped in the mutation-smoke gate slice without a wrapper naming
# its path, so agentic-selftests never ran it. See that bats file's header for the
# system under test.
#
# Exit codes:
#   0 — every bats test passed (and at least one ran)
#   1 — at least one bats test failed, zero tests ran, or the suite is missing
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    echo "test-mutation-smoke-bats: bats not on PATH (npm i -g bats). See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/mutation_smoke.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-mutation-smoke-bats: $BATS_FILE not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?
echo "$OUT"
PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${PASSED}  Failed: ${FAILED}"
if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then exit 1; fi
# Zero-run guard: a renamed/emptied suite must FAIL, not green-light a run in
# which nothing executed (the passed=0&&failed=0 fail-open class the
# historical-review ledger keeps re-finding in sibling wrappers, e.g. the
# Batch 16 #1175 finding on test-pr-status-watch-bats.sh).
if [ "$PASSED" -eq 0 ]; then
    echo "test-mutation-smoke-bats: FAIL — 0 bats tests ran from $BATS_FILE (suite renamed/empty?)" >&2
    exit 1
fi
exit 0
