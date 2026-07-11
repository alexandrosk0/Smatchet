#!/usr/bin/env bash
# test-plan-index-robustness-bats.sh — bats wrapper for
# tests/bats/plan_index_robustness.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. Required by
# the orphan-bats gate (every .bats needs a wrapper that names it). Covers the
# PR-7 plan-index robustness work: the shallow-clone guard
# (lib/plan-history-guard.sh) + the git-root path resolution in
# test-plan-index.sh. See the bats file header for the system under test.
#
# Exit codes:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing (BUILD.md § Dev-script CLI tools)
set -uo pipefail
# Resolve + verify the git root BEFORE cd: `cd "$(...)"` swallows an empty
# command-substitution (cd "" returns 0, leaving $PWD wherever the caller was),
# so a non-repo invocation would silently run against the wrong tree. Capture,
# require non-empty AND git success, else exit 2.
_GIT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 2
[ -n "$_GIT_ROOT" ] || exit 2
cd "$_GIT_ROOT" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    echo "test-plan-index-robustness-bats: bats not on PATH (npm i -g bats). See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/plan_index_robustness.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-plan-index-robustness-bats: $BATS_FILE not found" >&2
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
    echo "$(basename "$0" .sh): FAIL â the bats suite ran ZERO tests (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then exit 1; fi
exit 0
