#!/usr/bin/env bash
# test-git-leftover-audit-bats.sh — bats wrapper for tests/bats/git_leftover_audit.bats.
# Bucket A (CLI). Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
# Wraps the bats suite for the read-only branch/worktree leftover map (scripts/dev/git-leftover-audit.sh).
# Exit: 0 all pass · 1 a failure · 2 bats missing.
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2
if ! command -v bats >/dev/null 2>&1; then
    echo "test-git-leftover-audit-bats: bats not on PATH (npm i -g bats). See BUILD.md." >&2
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"; exit 2
fi
BATS_FILE="tests/bats/git_leftover_audit.bats"
[ -f "$BATS_FILE" ] || { echo "test-git-leftover-audit-bats: $BATS_FILE not found" >&2; echo "Passed: 0  Failed: 1"; exit 1; }
OUT="$(bats --tap "$BATS_FILE" 2>&1)"; RC=$?
echo "$OUT"
P=$(printf '%s\n' "$OUT" | grep -cE '^ok [0-9]+' || true)
F=$(printf '%s\n' "$OUT" | grep -cE '^not ok [0-9]+' || true)
echo "Passed: ${P}  Failed: ${F}"
{ [ "$F" -gt 0 ] || [ "$RC" -ne 0 ]; } && exit 1
exit 0
