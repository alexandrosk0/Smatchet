#!/usr/bin/env bash
# test-migrate-bugs-to-issues-bats.sh — bats wrapper for tests/bats/migrate_bugs_to_issues.bats (issue-triage Slice 3).
# Bucket A. Auto-enrolled by scripts/dev/test-all.sh. Emits the canonical Passed/Failed line.
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2
command -v bats >/dev/null 2>&1 || { echo "test-migrate-bugs-to-issues-bats: bats not on PATH" >&2; echo "Passed: 0  Failed: 0  (skipped — bats missing)"; exit 2; }
F="tests/bats/migrate_bugs_to_issues.bats"
[ -f "$F" ] || { echo "Passed: 0  Failed: 1"; exit 1; }
OUT="$(bats --tap "$F" 2>&1)"; RC=$?
echo "$OUT"
P=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
M=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${P}  Failed: ${M}"
{ [ "$M" -gt 0 ] || [ "$RC" -ne 0 ]; } && exit 1; exit 0
