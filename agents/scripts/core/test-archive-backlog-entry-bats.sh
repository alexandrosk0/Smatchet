#!/usr/bin/env bash
# test-archive-backlog-entry-bats.sh — bats wrapper for tests/bats/archive_backlog_entry.bats
# (per-entry-archival-breaks-relative-links, tooling P2).
# Bucket A. Auto-enrolled by scripts/dev/test-all.sh. Emits the canonical Passed/Failed line.
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2
command -v bats >/dev/null 2>&1 || { echo "test-archive-backlog-entry-bats: bats not on PATH" >&2; echo "Passed: 0  Failed: 0  (skipped — bats missing)"; exit 2; }
F="tests/bats/archive_backlog_entry.bats"
[ -f "$F" ] || { echo "Passed: 0  Failed: 1"; exit 1; }
OUT="$(bats --tap "$F" 2>&1)"; RC=$?
echo "$OUT"
PASSED=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
FAILED=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${PASSED}  Failed: ${FAILED}"
# Zero-run floor. A suite that parses to no tests at all (`1..0`, rc 0 — e.g. a
# syntax error that bats swallows, or every case skipped) would otherwise report
# `Passed: 0  Failed: 0` and exit GREEN: a gate that verifies nothing while
# looking identical to one that passed. That is the same false-green shape this
# suite's subject matter is about, so it fails closed here.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "test-archive-backlog-entry-bats: FAIL — suite produced ZERO test results (expected >0); treating as failure, not a pass." >&2
    exit 1
fi
{ [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; } && exit 1; exit 0
