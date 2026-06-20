#!/usr/bin/env bash
# test-coderabbit-current-head.sh — runs the coderabbit-current-head.sh
# --selftest (pure formatter fixtures, no gh / no network) and emits the
# canonical `Passed: N  Failed: M` line that scripts/dev/test-all.sh aggregates.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob (the helper
# itself is `coderabbit-current-head.sh`, NOT test-*.sh, so it would otherwise
# never run in CI).
#
# Exit codes:
#   0 — selftest passed
#   1 — selftest failed
#   2 — helper not found
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "test-coderabbit-current-head: cannot resolve repo root" >&2; exit 2; }
HELPER="$ROOT/scripts/dev/coderabbit-current-head.sh"
if [ ! -f "$HELPER" ]; then
    echo "test-coderabbit-current-head: $HELPER not found" >&2
    echo "Passed: 0  Failed: 1"
    exit 2
fi

OUT="$(bash "$HELPER" --selftest 2>&1)"
RC=$?
echo "$OUT"
if [ "$RC" -ne 0 ]; then
    echo "Passed: 0  Failed: 1"
    exit 1
fi
# The helper already prints its own `Passed: 1  Failed: 0`; re-emit a canonical
# line so the aggregator counts exactly one suite regardless of helper wording.
echo "Passed: 1  Failed: 0"
exit 0
