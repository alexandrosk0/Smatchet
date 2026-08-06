#!/usr/bin/env bash
# test-font-asset-resolve-bats.sh — bats wrapper for tests/bats/font_asset_resolve.bats.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob. Emits the
# canonical `Passed: N  Failed: M` line test-all.sh greps for. Mirrors
# test-about-buildinfo-bats.sh.
#
# The wrapped suite drives `cmake -P` over real `git worktree add` fixtures
# (the Font Awesome TTF is gitignored, so a linked worktree must fall back to
# the main worktree's copy) and self-skips when cmake is absent, so a
# cmake-less runner reports skips rather than failures.
#
# Exit codes follow the test-author convention:
#   0 — every bats test passed
#   1 — at least one bats test failed
#   2 — bats binary missing

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v bats >/dev/null 2>&1; then
    cat >&2 <<'EOF'
test-font-asset-resolve-bats: bats not on PATH.
  Install: npm i -g bats   (preferred — resolves on the bash-tool PATH)
           pacman -S bats  (MSYS2)
           brew install bats-core  (macOS)
EOF
    echo "Passed: 0  Failed: 0  (skipped — bats missing)"
    exit 2
fi

BATS_FILE="tests/bats/font_asset_resolve.bats"
if [ ! -f "$BATS_FILE" ]; then
    echo "test-font-asset-resolve-bats: $BATS_FILE not found" >&2
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
# FAILED-only gate would report as green — the fail-open shape. The cmake-less
# skip path still emits `ok N # skip`, so it clears the floor.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "test-font-asset-resolve-bats: no TAP results parsed from $BATS_FILE (expected >= 1)" >&2
    exit 1
fi

if [ "$FAILED" -gt 0 ] || [ "$RC" -ne 0 ]; then
    exit 1
fi
exit 0
