#!/usr/bin/env bash
# test-sync-issue-labels-bats.sh — bats wrapper for tests/bats/sync_issue_labels.bats.
#
# Bucket A (CLI). Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
# Wraps the bats suite for agents/scripts/project/sync-issue-labels.sh (issue-triage
# Slice 2) + emits the canonical `Passed: N  Failed: M` line test-all.sh greps for.
#
# Exit codes (test-author convention):
#   0 — every bats test passed
#   1 — at least one failed
#   2 — bats binary missing

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    echo "test-sync-issue-labels-bats: bats not on PATH (npm i -g bats). See BUILD.md." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/sync_issue_labels.bats"
[ -f "$BATS_FILE" ] || { echo "test-sync-issue-labels-bats: $BATS_FILE not found" >&2; echo "Passed: 0  Failed: 1"; exit 1; }

OUT="$(bats --tap "$BATS_FILE" 2>&1)"
RC=$?
echo "$OUT"
PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${PASSED}  Failed: ${FAILED}"
# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
{ [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; } && exit 1
exit 0
